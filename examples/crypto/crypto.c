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
        "-A 'freetoon-crypto/1.0' "
        "'https://api.coingecko.com/api/v3/simple/price?ids=%s&vs_currencies=%s&include_24hr_change=true'",
        ids, vs);
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

    if (load_config() != 0) { sleep(600); return 1; }

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
