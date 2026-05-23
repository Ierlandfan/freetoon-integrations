/*
 * findmy — "Find my device" integration for freetoon-lvgl.
 *
 * Shows where your trackers are (Apple AirTags via the FindMy / iCloud HA
 * integration, Tile/iTag/SmartTag, or any Home Assistant device_tracker).
 * Polls HA's REST API for each configured tracker entity, publishes the
 * primary tracker's current place on BoxTalk as the `findMyTag` service
 * (drives a home tile), and can raise an Inbox alert when a tracker leaves
 * home.
 *
 * "Location" here is the HA zone/state in words — "Thuis", a named zone, or
 * "Onderweg" (not_home). HA does the zone resolution; we just surface it.
 *
 * Same BoxTalk publish path + generic <alert> channel as crypto/openweather.
 *
 * Build:   make            # produces ./findmy
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
#define SERVICE_NAME    "findMyTag"
#define POLL_INTERVAL   60
#define REANNOUNCE_TICKS 8
#define CONFIG_PATH     "/mnt/data/integrations/findmy/findmy.conf"
#define MAX_TAGS        8
#define BUF_MAX         4096

static const char UUID[] = "findmy-c-5d1b";

static char ha_url[128]   = {0};   /* e.g. http://192.168.3.101:8123 */
static char ha_token[300] = {0};

typedef struct {
    char entity[96];     /* device_tracker.* */
    char label[24];      /* shown on the tile/alert */
    int  alert;          /* alert when this tag is not home */
} tag_t;
static tag_t tags[MAX_TAGS];
static int   tag_n = 0;

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void trim(char * s) {
    size_t n = strlen(s);
    while (n && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) s[--n]=0;
}

/* Config:
 *   HA_URL=http://<ha-host>:8123
 *   HA_TOKEN=<long-lived access token>
 *   TAG=<device_tracker.entity>,<label>,<on|off>
 */
static int load_config(void) {
    FILE * f = fopen(CONFIG_PATH, "r");
    if (!f) { fprintf(stderr, "findmy: no config at %s\n", CONFIG_PATH); return -1; }
    char line[512];
    tag_n = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char * eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0; char * v = eq + 1; trim(v);
        if (!strcmp(line, "HA_URL"))        snprintf(ha_url, sizeof ha_url, "%s", v);
        else if (!strcmp(line, "HA_TOKEN")) snprintf(ha_token, sizeof ha_token, "%s", v);
        else if (!strcmp(line, "TAG") && tag_n < MAX_TAGS) {
            char ent[96]="", lab[24]="", al[8]="";
            char * tok = strtok(v, ","); int fld = 0;
            while (tok) {
                switch (fld) { case 0: snprintf(ent,sizeof ent,"%s",tok); break;
                               case 1: snprintf(lab,sizeof lab,"%s",tok); break;
                               case 2: snprintf(al,sizeof al,"%s",tok); break; }
                tok = strtok(NULL, ","); fld++;
            }
            if (!ent[0]) continue;
            tag_t * t = &tags[tag_n];
            snprintf(t->entity, sizeof t->entity, "%s", ent);
            snprintf(t->label, sizeof t->label, "%s", lab[0] ? lab : ent);
            t->alert = (strstr(al,"on")!=NULL || strstr(al,"1")!=NULL || strstr(al,"yes")!=NULL);
            tag_n++;
        }
    }
    fclose(f);
    if (!ha_url[0] || !ha_token[0]) { fprintf(stderr, "findmy: HA_URL/HA_TOKEN not set\n"); return -1; }
    if (tag_n == 0) { fprintf(stderr, "findmy: no TAG= lines\n"); return -1; }
    return 0;
}

/* GET /api/states/<entity> and pull the "state" field (the zone/place). */
static int ha_state(const char * entity, char * out, size_t outsz) {
    char cmd[700];
    snprintf(cmd, sizeof cmd,
        "/usr/bin/curl -fsSL -k --max-time 10 --connect-timeout 6 "
        "-H 'Authorization: Bearer %s' '%s/api/states/%s'",
        ha_token, ha_url, entity);
    FILE * fp = popen(cmd, "r");
    if (!fp) return -1;
    static char body[BUF_MAX];
    size_t got = fread(body, 1, sizeof body - 1, fp);
    pclose(fp);
    body[got] = 0;
    if (got < 8) return -1;
    const char * p = strstr(body, "\"state\":");
    if (!p) return -1;
    p = strchr(p, ':'); if (!p) return -1; p++;
    while (*p == ' ' || *p == '"') p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < outsz) out[n++] = *p++;
    out[n] = 0;
    return 0;
}

/* Map HA's raw state to a friendly Dutch place string. */
static void pretty_place(const char * state, char * out, size_t outsz) {
    if (!state[0] || !strcmp(state, "unknown") || !strcmp(state, "unavailable"))
        snprintf(out, outsz, "onbekend");
    else if (!strcmp(state, "home"))     snprintf(out, outsz, "Thuis");
    else if (!strcmp(state, "not_home")) snprintf(out, outsz, "Onderweg");
    else snprintf(out, outsz, "%s", state);   /* a named zone */
}

static int send_frame(int fd, const char * xml) {
    size_t n = strlen(xml);
    if (send(fd, xml, n, MSG_NOSIGNAL) != (ssize_t)n) return -1;
    char nul = 0;
    return send(fd, &nul, 1, MSG_NOSIGNAL) == 1 ? 0 : -1;
}
static int bxt_connect(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(BXT_PORT);
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
        "<service type=\"%s\" version=\"1\"/></discovery>", UUID, SERVICE_NAME);
    return send_frame(fd, buf);
}
static void xml_clean(const char * in, char * out, size_t outsz) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < outsz; i++) {
        char c = in[i];
        if (c=='<'||c=='>'||c=='"'||c=='&') continue;
        out[j++] = c;
    }
    out[j] = 0;
}
static int notify_tag(int fd, const char * loc, const char * label, const char * alert) {
    char lc[64], la[24], al[160];
    xml_clean(loc, lc, sizeof lc);
    xml_clean(label, la, sizeof la);
    xml_clean(alert, al, sizeof al);
    char buf[768];
    snprintf(buf, sizeof buf,
        "<notify uuid=\"%s\" serviceid=\"urn:hcb-hae-com:serviceId:%s\">"
        "<loc>%s</loc><label>%s</label><alert>%s</alert></notify>",
        UUID, SERVICE_NAME, lc, la, al);
    return send_frame(fd, buf);
}

int main(void) {
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN);
    if (load_config() != 0) { sleep(600); return 1; }

    while (!g_stop) {
        int fd = bxt_connect();
        if (fd < 0) { fprintf(stderr, "findmy: BoxTalk connect failed\n"); sleep(10); continue; }
        if (announce(fd) != 0) { close(fd); sleep(2); continue; }
        fprintf(stderr, "findmy: announced %s, %d tag(s)\n", UUID, tag_n);

        int tick = 0;
        while (!g_stop) {
            char prim_loc[64] = "?", prim_label[24] = "-", alert[160] = "";
            for (int i = 0; i < tag_n; i++) {
                char st[64] = "", place[64];
                if (ha_state(tags[i].entity, st, sizeof st) != 0) continue;
                pretty_place(st, place, sizeof place);
                if (i == 0) {                       /* primary → tile */
                    snprintf(prim_loc, sizeof prim_loc, "%s", place);
                    snprintf(prim_label, sizeof prim_label, "%s", tags[i].label);
                }
                if (tags[i].alert && strcmp(st, "home") != 0 &&
                    strcmp(st, "unknown") != 0 && strcmp(st, "unavailable") != 0) {
                    char one[80];
                    snprintf(one, sizeof one, "%s%s: %s",
                             alert[0] ? " | " : "", tags[i].label, place);
                    strncat(alert, one, sizeof alert - strlen(alert) - 1);
                }
            }
            if (notify_tag(fd, prim_loc, prim_label, alert) != 0) {
                fprintf(stderr, "findmy: notify failed — reconnecting\n"); break;
            }
            fprintf(stderr, "findmy: %s = %s | alert=%s\n",
                    prim_label, prim_loc, alert[0] ? alert : "(none)");
            if (tick % REANNOUNCE_TICKS == 0 && tick > 0) if (announce(fd) != 0) break;
            tick++;
            sleep(POLL_INTERVAL);
        }
        close(fd);
    }
    return 0;
}
