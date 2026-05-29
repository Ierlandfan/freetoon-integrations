/*
 * crypto — cryptocurrency price tracker for freetoon-lvgl.
 *
 * Polls CoinGecko's free simple/price API every 2 minutes (one call for all
 * tracked coins), publishes the primary coin's price + 24h change on BoxTalk
 * as the `cryptoTicker` service (drives a home tile), and raises a freetoon
 * Inbox/banner alert when any coin crosses its configured threshold.
 *
 * Display mode is "both" by nature: the tile is always shown for the primary
 * coin, and per-coin alerts fire independently. Each coin's threshold,
 * direction (above/below) and whether it alerts at all are configured per
 * coin in crypto.conf.
 *
 * Alerts use the generic integration alert channel: we publish an <alert>
 * element in the notify frame; toonui (which owns the Toon's BoxTalk UUIDs)
 * turns a non-empty <alert> into a notification and clears it when empty.
 *
 * Same BoxTalk publish path as openweather/hello-solar — read those for the
 * framing details.
 *
 * Build:   make            # produces ./crypto
 *          make tarball    # packages for the marketplace catalog
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BXT_HOST        "127.0.0.1"
#define BXT_PORT        1337
#define SERVICE_NAME    "cryptoTicker"
#define POLL_INTERVAL   120          /* 2 min — well within CoinGecko's free rate limit */
#define REANNOUNCE_TICKS 5
#define CONFIG_PATH     "/mnt/data/integrations/crypto/crypto.conf"
#define MAX_COINS       8
#define BUF_MAX         8192
#define CRYPTO_DIR      "/mnt/data/integrations/crypto"
#define COINS_TSV       CRYPTO_DIR "/coins.tsv"     /* id<TAB>symbol<TAB>name, for the Settings search */
#define COINS_MAX_AGE   (7 * 24 * 3600)            /* refresh the coin list weekly */
#define HIST_DIR        CRYPTO_DIR "/hist"          /* per-coin/timeframe series for the graph screen */
#define HIST_MAX_PTS    240                         /* downsample target per series (plenty for an lv_chart) */

/* Timeframes the graph screen offers, as CoinGecko market_chart `days` values:
 * 24h / 7d / 30d / 1y / max. File names use the days token (max -> "max"). */
static const char * const TF_DAYS[] = { "1", "7", "30", "365", "max" };
#define TF_N ((int)(sizeof TF_DAYS / sizeof TF_DAYS[0]))
static int g_hist_rot = 0;   /* rotates one (coin,timeframe) refresh per poll cycle */

static const char UUID[] = "crypto-c-3f7a";

typedef struct {
    char   id[24];       /* CoinGecko id, e.g. "bitcoin" */
    char   sym[8];       /* display symbol, e.g. "BTC" */
    double threshold;    /* alert threshold in the vs currency */
    int    above;        /* 1 = alert when price >= threshold, 0 = when <= */
    int    alert;        /* 1 = alerts enabled for this coin */
    int    breaching;    /* runtime: 1 if currently past threshold (for hysteresis logging) */
} coin_t;

static coin_t coins[MAX_COINS];
static int    coin_n = 0;
static char   vs[8] = "eur";     /* vs currency */
static char   g_api_key[80] = ""; /* optional CoinGecko demo API key (KEY= in crypto.conf) — raises the free-tier rate limit so bulk history fetches don't 429 */

/* curl header carrying the optional demo key (empty string when unset). */
static const char * api_hdr(void) {
    static char h[120];
    if (g_api_key[0]) snprintf(h, sizeof h, "-H 'x-cg-demo-api-key: %s' ", g_api_key);
    else h[0] = 0;
    return h;
}

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* ===================================================================== */
/* Config                                                                */
/*   VS=eur                                                              */
/*   COIN=<coingecko-id>,<symbol>,<threshold>,<above|below>,<on|off>     */
/* ===================================================================== */
static void trim(char * s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
}

static int load_config(void) {
    FILE * f = fopen(CONFIG_PATH, "r");
    if (!f) { fprintf(stderr, "crypto: no config at %s\n", CONFIG_PATH); return -1; }
    char line[256];
    coin_n = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char * eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char * v = eq + 1;
        trim(v);
        if (strcmp(line, "VS") == 0) {
            snprintf(vs, sizeof vs, "%s", v);
            for (char * p = vs; *p; p++) *p = (char)tolower((unsigned char)*p);
        } else if (strcmp(line, "KEY") == 0) {
            snprintf(g_api_key, sizeof g_api_key, "%s", v);
        } else if (strcmp(line, "COIN") == 0 && coin_n < MAX_COINS) {
            /* id,sym,threshold,dir,alert */
            char id[24] = "", sym[8] = "", thr[24] = "", dir[12] = "", al[8] = "";
            char * tok = strtok(v, ",");  int fld = 0;
            while (tok) {
                switch (fld) {
                    case 0: snprintf(id,  sizeof id,  "%s", tok); break;
                    case 1: snprintf(sym, sizeof sym, "%s", tok); break;
                    case 2: snprintf(thr, sizeof thr, "%s", tok); break;
                    case 3: snprintf(dir, sizeof dir, "%s", tok); break;
                    case 4: snprintf(al,  sizeof al,  "%s", tok); break;
                }
                tok = strtok(NULL, ","); fld++;
            }
            if (!id[0]) continue;
            coin_t * c = &coins[coin_n];
            memset(c, 0, sizeof *c);
            snprintf(c->id, sizeof c->id, "%s", id);
            snprintf(c->sym, sizeof c->sym, "%s", sym[0] ? sym : id);
            c->threshold = atof(thr);
            c->above = (strstr(dir, "above") != NULL);
            c->alert = (strstr(al, "on") != NULL || strstr(al, "1") != NULL ||
                        strstr(al, "yes") != NULL);
            coin_n++;
        }
    }
    fclose(f);
    if (coin_n == 0) { fprintf(stderr, "crypto: no COIN= lines in %s\n", CONFIG_PATH); return -1; }
    return 0;
}

/* ===================================================================== */
/* JSON pluck (single field within a slice) — ASCII numbers only         */
/* ===================================================================== */
static int extract_double(const char * json, const char * field, double * out) {
    char needle[48];
    snprintf(needle, sizeof needle, "\"%s\"", field);
    const char * p = strstr(json, needle);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 'n') return 0;   /* null */
    *out = strtod(p, NULL);
    return 1;
}

/* Pull one coin's {price, change} out of the combined response. */
static int coin_values(const char * json, const char * id,
                       double * price, double * change) {
    char needle[32];
    snprintf(needle, sizeof needle, "\"%s\"", id);
    const char * p = strstr(json, needle);
    if (!p) return 0;
    const char * lb = strchr(p, '{');
    if (!lb) return 0;
    const char * rb = strchr(lb, '}');
    if (!rb) return 0;
    char slice[256];
    size_t n = (size_t)(rb - lb) + 1;
    if (n >= sizeof slice) n = sizeof slice - 1;
    memcpy(slice, lb, n); slice[n] = 0;

    int gp = extract_double(slice, vs, price);
    char chf[24];
    snprintf(chf, sizeof chf, "%s_24h_change", vs);
    if (!extract_double(slice, chf, change)) *change = 0;
    return gp;
}

/* ===================================================================== */
/* HTTP via popen(curl)                                                  */
/* ===================================================================== */
static int fetch_prices(char * out, size_t outsz) {
    char ids[256] = "";
    for (int i = 0; i < coin_n; i++) {
        if (i) strncat(ids, ",", sizeof ids - strlen(ids) - 1);
        strncat(ids, coins[i].id, sizeof ids - strlen(ids) - 1);
    }
    char cmd[640];
    snprintf(cmd, sizeof cmd,
        "/usr/bin/curl -fsSL -k --max-time 12 --connect-timeout 6 "
        "-A 'freetoon-crypto/1.0' %s"
        "'https://api.coingecko.com/api/v3/simple/price?ids=%s&vs_currencies=%s&include_24hr_change=true'",
        api_hdr(), ids, vs);
    FILE * fp = popen(cmd, "r");
    if (!fp) return -1;
    size_t got = 0;
    while (got < outsz - 1) {
        size_t k = fread(out + got, 1, outsz - 1 - got, fp);
        if (k == 0) break;
        got += k;
    }
    out[got] = 0;
    int rc = pclose(fp);
    return (rc == 0 && got > 0) ? 0 : -1;
}

/* ===================================================================== */
/* Coin-list cache + history pre-fetch (for the Settings search + graphs)  */
/* ===================================================================== */
static int file_age_s(const char * path) {           /* secs since mtime, -1 if missing */
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int)(time(NULL) - st.st_mtime);
}

/* curl a URL to <path>.tmp then atomically rename to <path>. */
static int curl_to_file(const char * url, const char * path) {
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    char cmd[1100];
    snprintf(cmd, sizeof cmd,
        "/usr/bin/curl -fsSL -k --max-time 40 --connect-timeout 8 "
        "-A 'freetoon-crypto/1.0' %s'%s' -o '%s'", api_hdr(), url, tmp);
    if (system(cmd) != 0) { remove(tmp); return -1; }
    struct stat st;
    if (stat(tmp, &st) != 0 || st.st_size < 8) { remove(tmp); return -1; }
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

/* Slurp a file into a malloc'd NUL-terminated buffer (caller frees). */
static char * slurp(const char * path, long * out_sz) {
    FILE * f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char * buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    if (out_sz) *out_sz = (long)got;
    return buf;
}

/* Fetch CoinGecko coins/list (~10k) and flatten to id<TAB>symbol<TAB>name.tsv
 * so the Settings search can grep it without a giant live fetch. Weekly. */
static void refresh_coins_list(void) {
    int age = file_age_s(COINS_TSV);
    if (age >= 0 && age < COINS_MAX_AGE) return;
    if (curl_to_file("https://api.coingecko.com/api/v3/coins/list", "/tmp/coins.json") != 0) {
        fprintf(stderr, "crypto: coins/list fetch failed\n"); return;
    }
    long sz = 0; char * buf = slurp("/tmp/coins.json", &sz);
    if (!buf) return;
    FILE * out = fopen(COINS_TSV ".tmp", "w");
    if (!out) { free(buf); return; }
    const char * p = buf; int n = 0;
    /* coins/list fields are in id,symbol,name order per object. */
    while ((p = strstr(p, "\"id\":\"")) != NULL) {
        p += 6; char id[64]; int k = 0;
        while (*p && *p != '"' && k < 63) id[k++] = *p++; id[k] = 0;
        char sym[40] = "", name[120] = "";
        const char * ps = strstr(p, "\"symbol\":\"");
        if (ps && ps - p < 100) { ps += 10; k = 0; while (*ps && *ps != '"' && k < 39) sym[k++] = *ps++; sym[k] = 0; }
        const char * pn = strstr(p, "\"name\":\"");
        if (pn && pn - p < 300) { pn += 8; k = 0; while (*pn && *pn != '"' && k < 119) name[k++] = *pn++; name[k] = 0; }
        if (id[0]) { fprintf(out, "%s\t%s\t%s\n", id, sym, name); n++; }
    }
    free(buf); fclose(out);
    if (n > 0) { rename(COINS_TSV ".tmp", COINS_TSV);
                 fprintf(stderr, "crypto: coins.tsv refreshed (%d coins)\n", n); }
    else remove(COINS_TSV ".tmp");
}

/* Fetch one coin's market_chart for `days`, parse the prices[[ts,price],...]
 * array and write a downsampled ts<TAB>price series to hist/<id>_<days>.tsv. */
static void refresh_history(const char * id, const char * days) {
    char url[320];
    snprintf(url, sizeof url,
        "https://api.coingecko.com/api/v3/coins/%s/market_chart?vs_currency=%s&days=%s",
        id, vs, days);
    if (curl_to_file(url, "/tmp/mc.json") != 0) return;
    long sz = 0; char * buf = slurp("/tmp/mc.json", &sz);
    if (!buf) return;
    const char * p = strstr(buf, "\"prices\"");
    if (p) p = strchr(p, '[');           /* opening bracket of the prices array */
    if (!p) { free(buf); return; }
    p++;
    /* First pass: count pairs to compute a downsample stride. */
    int total = 0; const char * q = p;
    while (*q && *q != ']') { if (*q == '[') total++; q++; }
    int stride = (total > HIST_MAX_PTS) ? (total / HIST_MAX_PTS) : 1;
    char path[300]; snprintf(path, sizeof path, "%s/%s_%s.tsv", HIST_DIR, id, days);
    char tmp[320];  snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE * out = fopen(tmp, "w");
    if (!out) { free(buf); return; }
    int i = 0, wrote = 0;
    q = p;
    while (*q && *q != ']') {
        if (*q == '[') {
            const char * pair = q + 1;
            char * end = NULL;
            double ts = strtod(pair, &end);          /* ms timestamp */
            if (end && *end == ',') {
                double price = strtod(end + 1, NULL);
                if ((i % stride) == 0) {
                    fprintf(out, "%lld\t%.6g\n", (long long)(ts / 1000.0), price);
                    wrote++;
                }
            }
            i++;
            const char * rb = strchr(q, ']');        /* skip to end of this pair */
            q = rb ? rb + 1 : q + 1;
        } else q++;
    }
    free(buf); fclose(out);
    if (wrote > 0) rename(tmp, path);
    else remove(tmp);
}

/* ===================================================================== */
/* Formatting                                                            */
/* ===================================================================== */
static const char * cur_sym(void) {
    if (!strcmp(vs, "eur")) return "€";  /* not used (non-ASCII) — see below */
    return "";
}

/* The tile font is ASCII-only, so use a currency letter prefix rather than a
 * symbol glyph. EUR/USD/GBP get a short tag; otherwise the upper-case code. */
static void cur_prefix(char * out, size_t osz) {
    if      (!strcmp(vs, "eur")) snprintf(out, osz, "EUR ");
    else if (!strcmp(vs, "usd")) snprintf(out, osz, "USD ");
    else if (!strcmp(vs, "gbp")) snprintf(out, osz, "GBP ");
    else { snprintf(out, osz, "%s ", vs); for (char*p=out;*p;p++)*p=(char)toupper((unsigned char)*p); }
}

/* Compact price: 58.2k / 1.23M / 0.42 — keeps the big tile number short. */
static void fmt_compact(double v, char * out, size_t osz) {
    if (v >= 1e6)      snprintf(out, osz, "%.2fM", v / 1e6);
    else if (v >= 1e3) snprintf(out, osz, "%.1fk", v / 1e3);
    else if (v >= 1)   snprintf(out, osz, "%.0f", v);
    else               snprintf(out, osz, "%.4f", v);
}

/* ===================================================================== */
/* BoxTalk                                                               */
/* ===================================================================== */
static int send_frame(int fd, const char * xml) {
    size_t n = strlen(xml);
    if (send(fd, xml, n, MSG_NOSIGNAL) != (ssize_t)n) return -1;
    char nul = 0;
    return send(fd, &nul, 1, MSG_NOSIGNAL) == 1 ? 0 : -1;
}

static int bxt_connect(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(BXT_PORT);
    inet_pton(AF_INET, BXT_HOST, &a.sin_addr);
    if (connect(fd, (struct sockaddr*)&a, sizeof a) != 0) { close(fd); return -1; }
    return fd;
}

static int announce(int fd) {
    char buf[512];
    snprintf(buf, sizeof buf,
        "<discovery nts=\"ssdp:alive\" uuid=\"%s\" "
        "type=\"urn:schemas-hcb-hae-com:device:thirdParty\" version=\"v\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
        "<service type=\"%s\" version=\"1\"/></discovery>",
        UUID, SERVICE_NAME);
    return send_frame(fd, buf);
}

static int notify_crypto(int fd, const char * price, const char * sub, const char * alert) {
    char buf[1024];
    snprintf(buf, sizeof buf,
        "<notify uuid=\"%s\" serviceid=\"urn:hcb-hae-com:serviceId:%s\">"
        "<price>%s</price><sub>%s</sub><alert>%s</alert>"
        "</notify>",
        UUID, SERVICE_NAME, price, sub, alert);
    return send_frame(fd, buf);
}

/* ===================================================================== */
/* Main loop                                                             */
/* ===================================================================== */
int main(void) {
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    mkdir(CRYPTO_DIR, 0755);
    mkdir(HIST_DIR, 0755);
    /* Fetch the coin list FIRST, unconditionally — the Settings coin-search
     * needs coins.tsv even on a fresh install before any coin is configured
     * (otherwise the picker is empty and you can never choose coins). */
    refresh_coins_list();

    if (load_config() != 0) {
        fprintf(stderr, "crypto: no coins configured yet — pick some in Settings -> Crypto\n");
        sleep(600); return 1;
    }

    static char json[BUF_MAX];

    while (!g_stop) {
        int fd = bxt_connect();
        if (fd < 0) { fprintf(stderr, "crypto: BoxTalk connect failed\n"); sleep(10); continue; }
        if (announce(fd) != 0) { close(fd); sleep(2); continue; }
        fprintf(stderr, "crypto: announced %s, %d coin(s), vs=%s\n", UUID, coin_n, vs);

        int tick = 0;
        while (!g_stop) {
            if (fetch_prices(json, sizeof json) == 0) {
                char pref[8]; cur_prefix(pref, sizeof pref);

                /* Primary coin → tile value + subtitle. */
                double pp = 0, pc = 0;
                char price_str[48] = "n/a", sub_str[64] = "";
                if (coin_values(json, coins[0].id, &pp, &pc)) {
                    char comp[24]; fmt_compact(pp, comp, sizeof comp);
                    snprintf(price_str, sizeof price_str, "%s%s", pref, comp);
                    snprintf(sub_str, sizeof sub_str, "%s  %+.1f%% 24h", coins[0].sym, pc);
                }

                /* Any coin past its threshold → alert text (joined). */
                char alert[256] = "";
                for (int i = 0; i < coin_n; i++) {
                    if (!coins[i].alert) continue;
                    double v = 0, ch = 0;
                    if (!coin_values(json, coins[i].id, &v, &ch)) continue;
                    int breach = coins[i].above ? (v >= coins[i].threshold)
                                                : (v <= coins[i].threshold);
                    if (breach) {
                        char thr[24], cur[24];
                        fmt_compact(coins[i].threshold, thr, sizeof thr);
                        fmt_compact(v, cur, sizeof cur);
                        /* e.g. "BTC <= EUR 50k (nu EUR 48.2k)" */
                        char one[96];
                        snprintf(one, sizeof one, "%s%s %s%s%s (nu %s%s)",
                                 alert[0] ? " | " : "",
                                 coins[i].sym, coins[i].above ? ">= " : "<= ",
                                 pref, thr, pref, cur);
                        strncat(alert, one, sizeof alert - strlen(alert) - 1);
                    }
                    coins[i].breaching = breach;
                }

                if (notify_crypto(fd, price_str, sub_str[0] ? sub_str : "-", alert) != 0) {
                    fprintf(stderr, "crypto: notify failed — reconnecting\n");
                    break;
                }
                fprintf(stderr, "crypto: %s | %s | alert=%s\n",
                        price_str, sub_str, alert[0] ? alert : "(none)");
            } else {
                fprintf(stderr, "crypto: API fetch failed — retrying next cycle\n");
            }

            /* Pre-fetch graph history: one (coin,timeframe) pair per cycle,
             * rotating through all coin_n*TF_N combinations, to stay gentle on
             * CoinGecko's free rate limit. Writes hist/<id>_<days>.tsv. */
            if (coin_n > 0) {
                int total = coin_n * TF_N;
                int idx = g_hist_rot % total;
                refresh_history(coins[idx / TF_N].id, TF_DAYS[idx % TF_N]);
                g_hist_rot++;
            }

            if (tick % REANNOUNCE_TICKS == 0 && tick > 0)
                if (announce(fd) != 0) break;
            tick++;
            sleep(POLL_INTERVAL);
        }
        close(fd);
    }
    (void)cur_sym;
    return 0;
}
