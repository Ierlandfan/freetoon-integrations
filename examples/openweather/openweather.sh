#!/bin/sh
# openweather — example freetoon-lvgl integration that hits an external HTTP API.
#
# Pulls current weather from openweathermap.org every 10 minutes and
# publishes it on BoxTalk as the `openWeather` service. Demonstrates:
#   - reading a config file (API key + city)
#   - using curl for an external HTTP call
#   - emitting multi-field notifies with both numeric and string values

set -eu

CONF=/mnt/data/integrations/openweather/openweather.conf
if [ ! -f "$CONF" ]; then
    echo "no config — write $CONF with API_KEY=… + CITY=…" >&2
    sleep 600
    exit 1
fi
# shellcheck disable=SC1090
. "$CONF"
[ -n "${API_KEY:-}" ] && [ "$API_KEY" != "PUT_YOUR_KEY_HERE" ] \
    || { echo "API_KEY not set" >&2; sleep 600; exit 1; }
CITY=${CITY:-Amsterdam,NL}

UUID="openweather-fixed"
SERVICE="openWeather"
FIFO=/tmp/openweather.fifo
rm -f "$FIFO"; mkfifo "$FIFO"
nc 127.0.0.1 1337 < "$FIFO" &
NC_PID=$!
exec 3>"$FIFO"

emit() { printf '%s\0' "$1" >&3; }
cleanup() { rm -f "$FIFO"; kill $NC_PID 2>/dev/null || true; }
trap cleanup EXIT INT TERM

# Announce
emit "<discovery nts=\"ssdp:alive\" uuid=\"$UUID\" \
type=\"urn:schemas-hcb-hae-com:device:thirdParty\" version=\"v\" \
xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\
<service type=\"$SERVICE\" version=\"1\"/></discovery>"

# Tiny JSON extractor — assumes one occurrence per field, ASCII-safe values.
json_field() {
    # $1 = JSON string, $2 = field name
    printf '%s' "$1" | sed -n "s/.*\"$2\"[ ]*:[ ]*\"\\([^\"]*\\)\".*/\\1/p"
}
json_num() {
    printf '%s' "$1" | sed -n "s/.*\"$2\"[ ]*:[ ]*\\([0-9.\\-]*\\).*/\\1/p"
}

while true; do
    URL="https://api.openweathermap.org/data/2.5/weather?q=$CITY&units=metric&appid=$API_KEY"
    JSON=$(curl -fsSL --max-time 15 "$URL" 2>/dev/null) || JSON=""
    if [ -n "$JSON" ]; then
        TEMP=$(json_num "$JSON" temp)
        HUMID=$(json_num "$JSON" humidity)
        DESC=$(json_field "$JSON" description)
        [ -n "$TEMP" ] || TEMP=0
        [ -n "$HUMID" ] || HUMID=0
        [ -n "$DESC" ] || DESC="—"
        emit "<notify uuid=\"$UUID\" \
serviceid=\"urn:hcb-hae-com:serviceId:$SERVICE\">\
<temp_c>$TEMP</temp_c><humidity>$HUMID</humidity>\
<description>$DESC</description></notify>"
        echo "$(date '+%F %T') temp=$TEMP humid=$HUMID desc='$DESC'"
    else
        echo "$(date '+%F %T') fetch failed"
    fi
    # Re-announce so toonui re-subscribes after restarts
    emit "<discovery nts=\"ssdp:alive\" uuid=\"$UUID\" \
type=\"urn:schemas-hcb-hae-com:device:thirdParty\" version=\"v\" \
xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\
<service type=\"$SERVICE\" version=\"1\"/></discovery>"

    sleep 600
done
