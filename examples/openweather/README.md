# openweather

API-driven integration example. Polls openweathermap.org every 10 minutes and publishes current temperature, humidity, and a short description on BoxTalk.

Shows three things the basic `hello-solar` template doesn't:

- **External HTTP(S) dependency**: shells out to `curl` (already on the Toon) instead of linking libcurl/libssl
- **Per-install secret**: API key lives in `openweather.conf`, not the binary
- **Non-trivial JSON parse**: pulls `temp` / `humidity` / `description` out of OpenWeather's nested response with a 30-line grep-style walker (no jq dependency)

## Setup

1. Sign up at https://openweathermap.org (free tier supports 60 calls/min — way more than this needs).
2. Install via the Marketplace screen on the Toon, or:
   ```sh
   curl -fsSL https://raw.githubusercontent.com/Ierlandfan/freetoon-integrations/main/install.sh | sh -s -- openweather
   ```
3. Drop your config in:
   ```sh
   cp /mnt/data/integrations/openweather/openweather.conf.example \
      /mnt/data/integrations/openweather/openweather.conf
   nano /mnt/data/integrations/openweather/openweather.conf
   # set API_KEY=... and CITY=...
   pkill -f openweather   # init respawns with the new config
   ```

## Build

```sh
make           # cross-compile for ARMv7
make tarball   # package binary + manifest + README + config template
```

## Output

The home tile (once tile-reassignment ships in toonui phase 2) shows the temperature as the big number and the description as the subtitle. Tile colour is sky-blue (`0x66bbdd`), icon is `cloud`.

## Quirks

- **API rate limits**: free tier is 60 calls/min, paid is higher. 10-minute polling sits well below that. Don't loosen it.
- **HTTPS CA store**: the Toon's `/etc/ssl/certs/ca-certificates.crt` is usually fine but old firmwares occasionally miss a CA. `curl -k` (insecure) is on by default to dodge that — production deployments should investigate the CA bundle and remove the flag.
- **City format**: stock `q=Name,CountryCode` works (`Amsterdam,NL` / `London,UK`). For ambiguous names use the city ID (`?id=…`) — see the OpenWeather docs.

## Adapting to other APIs

This file is the closest template to "external service → BoxTalk tile" you'll find. Copy the directory, change:

- `SERVICE_NAME` constant → your service id (matches `manifest.json` `service_id`)
- `fetch_weather_json()` → your endpoint URL + auth
- `extract_double()` / `extract_string()` → field names in YOUR API response
- `notify_weather()` → the field names YOUR tile expects (matches `manifest.json` `tile.value_field` / `tile.subtitle_field`)

Everything else (the announce → notify → reconnect loop) stays.
