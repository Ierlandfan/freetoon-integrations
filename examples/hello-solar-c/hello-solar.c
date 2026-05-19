/*
 * hello-solar — minimal freetoon-lvgl integration written in C.
 *
 * Connects to BoxTalk on 127.0.0.1:1337, announces itself as
 * `solarProduction`, emits a notify every 10 s with a fake power
 * reading. Self-contained — no libraries beyond libc. Cross-compile
 * once for armv7-hardfloat and ship the binary in the tarball; the
 * Toon doesn't need a build toolchain.
 *
 * Pattern is identical to the shell version in `../hello-solar/`. Read
 * that one first if you're new to the BoxTalk protocol — this is the
 * same flow with more boilerplate. Use the C version when:
 *   - you want a single statically-linked binary (no shell / nc deps)
 *   - your data source is something C handles natively (e.g. mmap'd
 *     hardware, libudev events, fast polling at sub-second rates)
 *   - the integration needs to run as a long-lived daemon with
 *     deterministic timing rather than `sleep 10` drift
 *
 * Build (on a host with the linaro armhf toolchain installed):
 *   make           # produces ./hello-solar
 *
 * Or compile manually:
 *   arm-linux-gnueabihf-gcc -O2 -static-libgcc -o hello-solar hello-solar.c
 */

#include <arpa/inet.h>
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

#define BXT_HOST       "127.0.0.1"
#define BXT_PORT       1337
#define SERVICE_NAME   "solarProduction"
#define POLL_INTERVAL  10            /* seconds between notify frames */
#define REANNOUNCE_EVERY 6           /* re-discovery every N notify cycles */

/* Pick a UUID that's distinctive enough to spot in toonui's BoxTalk log,
 * yet stable across restarts so subscribers don't rebind every cycle.
 * Real integrations should generate one once at install time and persist
 * it in their /mnt/data/integrations/<id>/ directory. */
static const char UUID[] = "hello-solar-c-7f4e1a08";

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* BoxTalk wire format: XML text + a single NUL byte (0x00) per frame.
 * The hub buffers until it sees the NUL, so EVERY send_frame call must
 * include the terminator. Forgetting it = silent message that never
 * reaches subscribers. */
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

static int notify_reading(int fd, int power_w, int today_kwh) {
    char buf[512];
    snprintf(buf, sizeof buf,
        "<notify uuid=\"%s\" "
        "serviceid=\"urn:hcb-hae-com:serviceId:%s\">"
        "<power_w>%d</power_w><today_kwh>%d</today_kwh></notify>",
        UUID, SERVICE_NAME, power_w, today_kwh);
    return send_frame(fd, buf);
}

int main(void) {
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    /* Stay alive across hub restarts. Init's respawn handles process
     * crashes; this inner loop handles transient connection failures
     * (which would otherwise need a full process exit + respawn cycle). */
    while (!g_stop) {
        int fd = bxt_connect();
        if (fd < 0) {
            fprintf(stderr, "hello-solar: connect failed: %s\n", strerror(errno));
            sleep(5);
            continue;
        }
        if (announce(fd) != 0) { close(fd); sleep(2); continue; }
        fprintf(stderr, "hello-solar: announced as %s on fd=%d\n", UUID, fd);

        int tick = 0;
        while (!g_stop) {
            /* Fake reading — sine-ish wave 0..3000 W so the tile shows
             * movement in toonui. Real integrations replace this block
             * with whatever their sensor / API gives. */
            int power_w   = (tick % 30) * 100;
            int today_kwh = (tick / 360) + 5;

            if (notify_reading(fd, power_w, today_kwh) != 0) {
                fprintf(stderr, "hello-solar: notify send failed — reconnecting\n");
                break;
            }

            if (tick % REANNOUNCE_EVERY == 0 && tick > 0) {
                /* Re-announce so toonui can re-subscribe across restarts
                 * without us having to detect that on this side. */
                if (announce(fd) != 0) break;
            }
            tick++;
            sleep(POLL_INTERVAL);
        }
        close(fd);
    }
    return 0;
}
