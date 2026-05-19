#!/bin/sh
# hello-solar — minimal freetoon-lvgl integration example.
#
# Connects to the BoxTalk hub on 127.0.0.1:1337, announces itself as a
# solarProduction service, then emits notify frames every 10 s with a
# fake power reading. Re-uses init's respawn semantics — if the netcat
# pipe drops, the script exits and init brings us back.
#
# All the moving parts are intentionally written in POSIX shell + busybox
# nc so anyone reading this can copy + modify without a build step. Once
# you understand the pattern, port to your language of choice.

set -eu

UUID="hello-solar-$(cat /proc/sys/kernel/random/uuid 2>/dev/null \
                   || awk 'BEGIN{srand(); printf "%08x",rand()*1e9}')"
SERVICE="solarProduction"
BXT_HOST=127.0.0.1
BXT_PORT=1337

# Use a FIFO so we can keep one nc open and dribble frames into it over
# the lifetime of the script. NUL-terminate each frame — that's the
# BoxTalk wire format (XML + NUL).
FIFO=/tmp/hello-solar.fifo
rm -f "$FIFO"
mkfifo "$FIFO"

# Background netcat — keep the TCP socket open. When the hub disconnects
# (toonui restart, hub crash), nc exits, the FIFO sees EOF, and our main
# loop's printf hits EPIPE → we exit → init respawns us.
nc "$BXT_HOST" "$BXT_PORT" < "$FIFO" &
NC_PID=$!
exec 3>"$FIFO"

emit() {
    # printf with %s then a literal '\0' NUL byte — the BoxTalk framing
    # delimiter. busybox printf supports \0 inside the format string.
    printf '%s\0' "$1" >&3
}

cleanup() {
    rm -f "$FIFO"
    kill $NC_PID 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# 1) Discovery announce — tells the hub we exist + what service we offer.
# The `type` doesn't have to match a registered schema; the `serviceid`
# inside notifies is what subscribers actually filter on.
emit "<discovery nts=\"ssdp:alive\" uuid=\"$UUID\" \
type=\"urn:schemas-hcb-hae-com:device:thirdParty\" version=\"v\" \
xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\
<service type=\"$SERVICE\" version=\"1\"/></discovery>"

# 2) Refresh loop — emit one notify every 10 s. Real integrations would
# pull from an HTTP API, parse JSON, etc. Here we just sine-wave a
# pretend power value 0..3000 W so the tile shows movement.
i=0
while true; do
    # Bash arithmetic via $(()) — busybox sh supports it. Sine wave is
    # cheaper than the cost of doing it externally with bc/awk.
    POWER=$(( (i % 30) * 100 ))
    KWH=$(( (i / 360) + 5 ))   # fake total kWh today, bumps every hour

    emit "<notify uuid=\"$UUID\" \
serviceid=\"urn:hcb-hae-com:serviceId:$SERVICE\">\
<power_w>$POWER</power_w><today_kwh>$KWH</today_kwh></notify>"

    # Re-announce every minute so toonui can re-subscribe after a restart.
    if [ $((i % 6)) -eq 0 ]; then
        emit "<discovery nts=\"ssdp:alive\" uuid=\"$UUID\" \
type=\"urn:schemas-hcb-hae-com:device:thirdParty\" version=\"v\" \
xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\
<service type=\"$SERVICE\" version=\"1\"/></discovery>"
    fi

    i=$((i + 1))
    sleep 10
done
