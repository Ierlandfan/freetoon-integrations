/*
 * oepi-watermeter — freetoon-lvgl integration for the @oepi-loepi
 * ESP/Wemos water meter (https://github.com/oepi-loepi/water_ESP_part).
 *
 * The Wemos D1 (flashed with ESPEasy + the oepi-loepi water sketch) counts
 * 1 pulse per liter and serves a JSON blob at:
 *
 *     GET http://<ESP_IP>/water.html
 *     {"waterflow":"0","waterquantity":"1294748","today":"48",
 *      "currentBatch":"0","breakdetect":"0","leakdetect":"0","RSSI":"-29",
 *      "version":"1.4.39","update":"0","pulselength":13693","pulsetime":"2186"}
 *
 *   waterflow      current flow, L/min
 *   waterquantity  cumulative meter total, LITERS   (/1000 -> m3)
 *   today          usage since midnight, liters
 *   leakdetect     1 = leak suspected
 *   breakdetect    1 = pipe-break suspected
 *
 * NOTE: the ESP JSON is slightly malformed near `pulselength` (a missing
 * opening quote), so we never strict-parse the whole document — we pluck
 * only the fields we need, each of which sits *before* the broken one.
 *
 * Publishes on BoxTalk as the `oepiWater` service:
 *   <m3>      cumulative total in m3 (3 decimals)
 *   <flow>    current flow L/min
 *   <today_l> liters since midnight
 *   <leak>    0/1
 *
 * Bind it to the Water home-tile slot (Settings -> Tiles, or long-press the
 * Water tile). manifest.json maps value=<m3>, subtitle=<flow>.
 *
 * Build:
 *   make            # ./oepi-watermeter
 *   make tarball    # package for the marketplace catalog
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BXT_HOST         "127.0.0.1"
#define BXT_PORT         1337
#define SERVICE_NAME     "oepiWater"
#define POLL_INTERVAL    5            /* seconds — flow is live-ish, keep it snappy */
#define REANNOUNCE_TICKS 60           /* re-announce every 60 polls (~5 min) */
#define CONFIG_PATH      "/mnt/data/integrations/oepi-watermeter/oepi-watermeter.conf"
#define BUF_MAX          2048

static const char UUID[] = "oepi-watermeter-c-1f4a";

static char esp_ip[64] = {0};

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* ===================================================================== */
/* Config — KEY=value lines. Only ESP_IP is required.                     */
/* ===================================================================== */
static int load_config(void) {
    FILE * f = fopen(CONFIG_PATH, "r");
    if (!f) { fprintf(stderr, "oepi-watermeter: no config at %s\n", CONFIG_PATH); return -1; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char * eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char * v = eq + 1;
        size_t n = strlen(v);
        while (n && (v[n-1] == '\n' || v[n-1] == '\r' || v[n-1] == ' ')) v[--n] = 0;
        if (strcmp(line, "ESP_IP") == 0) snprintf(esp_ip, sizeof esp_ip, "%s", v);
    }
    fclose(f);
    if (!esp_ip[0] || strcmp(esp_ip, "PUT_ESP_IP_HERE") == 0) {
        fprintf(stderr, "oepi-watermeter: ESP_IP not set in %s\n", CONFIG_PATH);
        return -1;
    }
    return 0;
}

/* ===================================================================== */
/* Field pluck — values are quoted strings: "field":"value". Returns the  */
/* numeric value (0 if absent). Tolerant of the malformed pulselength     */
/* field because we only ask for fields that precede it.                  */
/* ===================================================================== */
static int extract_num(const char * json, const char * field, double * out) {
    char needle[48];
    snprintf(needle, sizeof needle, "\"%s\"", field);
    const char * p = strstr(json, needle);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;   /* skip ws + opening quote */
    if (!*p) return 0;
    *out = strtod(p, NULL);
    return 1;
}

/* ===================================================================== */
/* HTTP via popen(curl) — no libcurl dependency                           */
/* ===================================================================== */
static int fetch_water_json(char * out, size_t outsz) {
    char cmd[256];
    snprintf(cmd, sizeof cmd,
        "/usr/bin/curl -fsS --max-time 6 --connect-timeout 4 "
        "-A 'oepi-watermeter/1.0' 'http://%s/water.html'",
        esp_ip);
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
/* BoxTalk — connect / announce / notify                                  */
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

static int notify_water(int fd, double m3, double flow, double today_l, int leak) {
    char buf[512];
    snprintf(buf, sizeof buf,
        "<notify uuid=\"%s\" serviceid=\"urn:hcb-hae-com:serviceId:%s\">"
        "<m3>%.3f</m3>"
        "<flow>%.1f</flow>"
        "<today_l>%.0f</today_l>"
        "<leak>%d</leak>"
        "</notify>",
        UUID, SERVICE_NAME, m3, flow, today_l, leak);
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
        /* Long sleep before exit so init doesn't hot-loop us while the
         * user is still entering the ESP IP. */
        sleep(600);
        return 1;
    }

    static char json[BUF_MAX];

    while (!g_stop) {
        int fd = bxt_connect();
        if (fd < 0) {
            fprintf(stderr, "oepi-watermeter: BoxTalk connect failed: %s\n", strerror(errno));
            sleep(10);
            continue;
        }
        if (announce(fd) != 0) { close(fd); sleep(2); continue; }
        fprintf(stderr, "oepi-watermeter: announced as %s (esp=%s)\n", UUID, esp_ip);

        int tick = 0;
        while (!g_stop) {
            if (fetch_water_json(json, sizeof json) == 0) {
                double quantity_l = 0, flow = 0, today_l = 0, leak = 0;
                int got_q = extract_num(json, "waterquantity", &quantity_l);
                extract_num(json, "waterflow", &flow);
                extract_num(json, "today",     &today_l);
                extract_num(json, "leakdetect", &leak);
                if (got_q) {
                    double m3 = quantity_l / 1000.0;
                    if (notify_water(fd, m3, flow, today_l, (int)leak) != 0) {
                        fprintf(stderr, "oepi-watermeter: notify failed - reconnecting\n");
                        break;
                    }
                    fprintf(stderr, "oepi-watermeter: %.3f m3  flow=%.1f L/min  today=%.0f L  leak=%d\n",
                            m3, flow, today_l, (int)leak);
                } else {
                    fprintf(stderr, "oepi-watermeter: parse failed: %.120s\n", json);
                }
            } else {
                fprintf(stderr, "oepi-watermeter: fetch from %s failed - retrying\n", esp_ip);
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
