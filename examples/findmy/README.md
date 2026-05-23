# Find my device

A freetoon-lvgl marketplace integration that shows where your trackers are,
using **Home Assistant** as the location source.

Works with anything HA exposes as a `device_tracker`:

- **Apple AirTags** via the FindMy / iCloud HA integration (the iOS equivalent
  of an item tracker)
- **Tile / iTag / Samsung SmartTag** trackers
- phones, watches, or any other `device_tracker`

The home tile shows the **primary** tracker's current place — `Thuis`, a named
HA zone (e.g. `Werk`, `School`), or `Onderweg` (not home). Each tracker can
raise a Toon **Inbox/banner alert** when it leaves home (per-tracker toggle).

> Location is HA's zone resolution in words. For a full map of a person's GPS,
> the built-in **Family/Life360 tile** opens a map when tapped.

## Install

Settings → **Marketplace** → **Find my device** → Install, then configure:

```sh
cp /mnt/data/integrations/findmy/findmy.conf.example \
   /mnt/data/integrations/findmy/findmy.conf
vi /mnt/data/integrations/findmy/findmy.conf      # HA URL + token + TAG lines
pkill -f integrations/findmy/findmy               # restart
```

Bind it to a home tile in Settings → **Tiles**.

## Config

See [`findmy.conf.example`](findmy.conf.example):

```
HA_URL=http://192.168.3.101:8123
HA_TOKEN=<long-lived access token>
TAG=device_tracker.airtag_keys,Sleutels,on
TAG=device_tracker.airtag_wallet,Portemonnee,off
```

Create the token in HA → your profile → **Long-Lived Access Tokens**.

| Field | Meaning |
|---|---|
| `HA_URL` | Base URL of your Home Assistant. |
| `HA_TOKEN` | Long-lived access token (Bearer). |
| `TAG` entity | A `device_tracker.*` entity id. |
| `TAG` label | Short name for the tile/alert. |
| `TAG` alert | `on` / `off` — alert when this tracker isn't home. |

## How it works

A small C daemon polls HA's REST API (`/api/states/<entity>`) for each tracker
every 60 s and publishes on the BoxTalk service `findMyTag`:

```xml
<notify uuid="findmy-c-5d1b" serviceid="urn:hcb-hae-com:serviceId:findMyTag">
  <loc>Thuis</loc>
  <label>Sleutels</label>
  <alert>Portemonnee: Onderweg</alert>
</notify>
```

`loc`/`label` drive the tile; a non-empty `alert` is surfaced by toonui as a
notification (the generic `alert_field` channel) and cleared when empty.

## Build

```sh
make            # cross-compile → ./findmy (ARMv7 hardfloat)
make tarball    # package for the catalog
```
