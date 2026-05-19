# openweather

Polls openweathermap.org every 10 minutes and publishes current temperature, humidity, and short description on BoxTalk.

Demonstrates how to handle an integration that depends on an **external API key** — the installer drops a config-file template, the script reads it on every poll cycle.

## Setup

1. Sign up at https://openweathermap.org (free tier supports 60 calls / minute).
2. Install: `curl -fsSL https://raw.githubusercontent.com/Ierlandfan/freetoon-integrations/main/install.sh | sh -s -- openweather`
3. Edit `/mnt/data/integrations/openweather/openweather.conf`:
   ```
   API_KEY=your-actual-key-here
   CITY=Amsterdam,NL
   ```
4. Restart the integration: `pkill -f openweather.sh` — init respawns it with the new config.

## Output

The home tile shows current temp as the big value and the description as the subtitle. Tile colour is sky-blue (`0x66bbdd`), icon is the cloud glyph.

## Quirks

- The shell-side JSON parser is **single-field-per-line** brittle. If openweathermap ever changes the response shape this will silently start emitting zeros. Real integrations should use a proper JSON parser (`jq`, or rewrite in Python/Go).
- The 10-minute poll interval respects the free-tier rate limit. Don't bring it below 60 s without upgrading your plan.
- HTTPS via curl needs the CA store at `/etc/ssl/certs/ca-certificates.crt` to be present (it is on stock Toon). If you see "SSL certificate problem" in the logs, add `-k` to the curl invocation as a workaround.
