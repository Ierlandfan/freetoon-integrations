# Water meter (oepi-loepi)

A freetoon-lvgl marketplace integration for the **@oepi-loepi** ESP/Wemos
water meter — the Wemos D1 (flashed with ESPEasy + the oepi-loepi water
sketch) that counts **1 pulse per liter** off an analog meter and serves a
JSON page at `http://<ESP_IP>/water.html`.

See the hardware/firmware project:
- https://github.com/oepi-loepi/water_ESP_part (the ESP firmware)
- https://github.com/oepi-loepi/toonWater (the original Toon qt-gui app)

## What it does

A small C daemon (no libcurl — it shells out to `curl`) polls the ESP every
5 s, reads:

| ESP JSON field  | meaning                | published as |
|-----------------|------------------------|--------------|
| `waterquantity` | cumulative total, L    | `<m3>` (÷1000) |
| `waterflow`     | current flow, L/min    | `<flow>` |
| `today`         | usage since midnight, L| `<today_l>` |
| `leakdetect`    | 1 = leak suspected     | `<leak>` |

…and publishes them on BoxTalk as the `oepiWater` service. Bind it to the
**Water** home tile (Settings -> Tiles, or long-press the Water tile):
value = m3 total, subtitle = L/min flow.

> The ESP's JSON has a known quirk near `pulselength` (a missing quote).
> This daemon plucks only the fields it needs — all of which come *before*
> the broken one — so it parses cleanly regardless.

## Install

From the freetoon **Settings -> Marketplace**, pick "Water meter
(oepi-loepi)". Then set the ESP's LAN IP:

```sh
# on the Toon
vi /mnt/data/integrations/oepi-watermeter/oepi-watermeter.conf
#   ESP_IP=192.168.x.y
pkill -f oepi-watermeter      # init respawns it with the new IP
```

If `ESP_IP` is left at the placeholder the daemon idles (so it's safe to
install before the meter is wired up).

## Build from source

```sh
make            # produces ./oepi-watermeter (armv7 hardfloat)
make tarball    # oepi-watermeter.tar.gz for the catalog
```

License: MIT.
