/*
 * openweather — API-driven freetoon-lvgl integration example.
 *
 * Polls openweathermap.org every 10 minutes via curl, parses temperature
 * + humidity + a short description out of the response, publishes them
 * on BoxTalk as the `openWeather` service. Demonstrates how to handle
 * an integration with:
 *   - external HTTP(S) dependency (via popen("curl ...", "r"))
 *   - per-install secret (the OpenWeather API key, in a config file)
 *   - parsing a non-trivial JSON response without a real JSON library
 *
 * Same BoxTalk publish path as hello-solar — read that one first for
 * the framing details. This file's interesting parts:
 *   - load_config()        — config-file format, where to drop it
 *   - fetch_weather_json() — popen-based HTTP fetch
 *   - extract_*()          — single-field grep-style JSON walk
 *   - main loop            — error handling, reconnect, retry cadence
 *
 * Build:
 *   make             # produces ./openweather
 *   make tarball     # packages openweather + manifest + README for the marketplace catalog
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
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define BXT_HOST        "127.0.0.1"
#define BXT_PORT        1337
#define SERVICE_NAME    "openWeather"
#define POLL_INTERVAL   600           /* 10 min between API calls — fits the free-tier rate limit */
#define REANNOUNCE_TICKS 6
#define CONFIG_PATH     "/mnt/data/integrations/openweather/openweather.conf"
#define BUF_MAX         8192

static const char UUID[] = "openweather-c-9b2e";

static char api_key[64] = {0};
static char city[64] = "Amsterdam,NL";

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* ===================================================================== */
/* Config — KEY=value lines in /mnt/data/integrations/openweather/...    */
/* ===================================================================== */
static int load_config(void) {
    FILE * f = fopen(CONFIG_PATH, "r");
    if (!f) { fprintf(stderr, "openweather: no config at %s\n", CONFIG_PATH); return -1; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char * eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char * v = eq + 1;
        size_t n = strlen(v);
        while (n && (v[n-1] == '\n' || v[n-1] == '\r' || v[n-1] == ' ')) v[--n] = 0;
        if (strcmp(line, "API_KEY") == 0) snprintf(api_key, sizeof api_key, "%s", v);
        else if (strcmp(line, "CITY") == 0) snprintf(city, sizeof city, "%s", v);
    }
    fclose(f);
    if (!api_key[0] || strcmp(api_key, "PUT_YOUR_KEY_HERE") == 0) {
        fprintf(stderr, "openweather: API_KEY not set in %s\n", CONFIG_PATH);
        return -1;
    }
    return 0;
}

/* ===================================================================== */
/* JSON helpers — single-occurrence pluck, ASCII-safe values             */
/* ===================================================================== */
static int extract_double(const char * json, const char * field, double * out) {
    char needle[64];
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

static int extract_string(const char * json, const char * field,
                          char * out, size_t outsz) {
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", field);
    const char * p = strstr(json, needle);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < outsz) {
        if (*p == '\\' && p[1]) { p++; }
        out[n++] = *p++;
    }
    out[n] = 0;
    return 1;
}

/* ===================================================================== */
/* HTTP via popen(curl) — keeps this binary off libcurl + libssl entirely */
/* ===================================================================== */
static int fetch_weather_json(char * out, size_t outsz) {
    char cmd[1024];
    /* -k accepts the system CA-store mismatches some Toon firmwares have;
     * --max-time 12 caps the whole fetch; -A is just polite. URL-escaping
     * isn't needed for typical city names (comma is allowed in path),
     * but be cautious if you stuff arbitrary user input here. */
    snprintf(cmd, sizeof cmd,
        "/usr/bin/curl -fsSL -k --max-time 12 --connect-timeout 6 "
        "-A 'openweather-c/1.0' "
        "'https://api.openweathermap.org/data/2.5/weather?q=%s&units=metric&appid=%s'",
        city, api_key);
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
/* BoxTalk — connect / announce / notify (identical pattern to hello-solar)*/
/* ===================================================================== */
static int send_frame(int fd, const char * xml) {
    size_t n = strlen(xml);
    if (send(fd, xml, n, MSG_NOSIGNAL) != (ssize_t)n) return -1;
    char nul = 0;
    if (send(fd, &nul, 1, MSG_NOSIGNAL) != 1) return -1;
    return 0;
}

static int bxt_connect(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(BXT_PORT);
    inet_pton(AF_INET, BXT_HOST, &a.sin_addr);
    if (connect(fd, (struct sockaddr*)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
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

static int notify_weather(int fd, double temp_c, double humidity,
                          const char * description) {
    char buf[1024];
    /* Strip any <>" chars from description so we don't corrupt the XML.
     * Field names match what toonui's manifest.json tile.value_field /
     * subtitle_field point at. */
    char desc_clean[64];
    size_t j = 0;
    for (size_t i = 0; description[i] && j + 1 < sizeof desc_clean; i++) {
        char c = description[i];
        if (c == '<' || c == '>' || c == '"' || c == '&') continue;
        desc_clean[j++] = c;
    }
    desc_clean[j] = 0;

    snprintf(buf, sizeof buf,
        "<notify uuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:%s\">"
        "<temp_c>%.1f</temp_c>"
        "<humidity>%.0f</humidity>"
        "<description>%s</description>"
        "</notify>",
        UUID, SERVICE_NAME, temp_c, humidity, desc_clean);
    return send_frame(fd, buf);
}

/* ===================================================================== */
/* Main loop                                                             */
/* ===================================================================== */
int main(void) {
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    if (load_config() != 0) {
        /* Sleep a long while before exit so init doesn't respawn us in a
         * tight loop while the user is still editing openweather.conf. */
        sleep(600);
        return 1;
    }

    static char json[BUF_MAX];

    while (!g_stop) {
        int fd = bxt_connect();
        if (fd < 0) {
            fprintf(stderr, "openweather: BoxTalk connect failed: %s\n", strerror(errno));
            sleep(10);
            continue;
        }
        if (announce(fd) != 0) { close(fd); sleep(2); continue; }
        fprintf(stderr, "openweather: announced as %s (city=%s)\n", UUID, city);

        int tick = 0;
        while (!g_stop) {
            if (fetch_weather_json(json, sizeof json) == 0) {
                double temp_c = 0, humidity = 0;
                char desc[64] = "";
                int got_temp = extract_double(json, "temp",        &temp_c);
                int got_hum  = extract_double(json, "humidity",    &humidity);
                extract_string(json, "description", desc, sizeof desc);
                if (got_temp || got_hum) {
                    if (notify_weather(fd, temp_c, humidity,
                                       desc[0] ? desc : "-") != 0) {
                        fprintf(stderr, "openweather: notify failed - reconnecting\n");
                        break;
                    }
                    fprintf(stderr, "openweather: %s temp=%.1f humid=%.0f desc=%s\n",
                            city, temp_c, humidity, desc);
                } else {
                    fprintf(stderr, "openweather: parse failed on response head: %.120s\n", json);
                }
            } else {
                fprintf(stderr, "openweather: API fetch failed - retrying next cycle\n");
            }

            if (tick % REANNOUNCE_TICKS == 0 && tick > 0) {
                if (announce(fd) != 0) break;
            }
            tick++;
            sleep(POLL_INTERVAL);
        }
        close(fd);
    }
    return 0;
}
