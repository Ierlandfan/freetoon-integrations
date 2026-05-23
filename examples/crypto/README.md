# Crypto tracker

A freetoon-lvgl marketplace integration that tracks cryptocurrency prices via
[CoinGecko](https://www.coingecko.com/)'s free public API (no key required).

- **Tile** — the primary coin's price (compact, e.g. `EUR 58.2k`) plus its 24h
  change (`BTC  +1.2% 24h`).
- **Alerts** — each tracked coin can raise a Toon Inbox/banner notification when
  it crosses a threshold you set, in either direction (above/below), toggleable
  per coin. The alert clears automatically when the price comes back.
- **"Both" by design** — the tile is always live *and* alerts fire
  independently, which is the behaviour requested: see the value on the tile and
  get notified on a threshold.

Multiple coins are polled in a single API call; the first `COIN=` line is the
one shown on the tile, and all coins drive alerts.

## Install

Settings → **Marketplace** → **Crypto tracker** → Install. Then edit the config:

```sh
cp /mnt/data/integrations/crypto/crypto.conf.example \
   /mnt/data/integrations/crypto/crypto.conf
vi /mnt/data/integrations/crypto/crypto.conf
pkill -f integrations/crypto/crypto      # restart to pick up the config
```

Bind it to a home tile in Settings → **Tiles**.

## Config

See [`crypto.conf.example`](crypto.conf.example). Format:

```
VS=eur
COIN=bitcoin,BTC,50000,below,on
COIN=ethereum,ETH,4000,above,off
```

| Field | Meaning |
|---|---|
| `VS` | Fiat currency (CoinGecko code: `eur`, `usd`, `gbp`, …). |
| `COIN` id | CoinGecko coin id (`bitcoin`, `ethereum`, `solana`, …). |
| `COIN` symbol | Short label for the tile/alert (`BTC`). |
| `COIN` threshold | Price (in `VS`) that triggers the alert. |
| `COIN` direction | `above` or `below`. |
| `COIN` alert | `on` / `off` — whether this coin alerts. |

## How it works

A small C daemon (`crypto`) shells out to `curl` for the CoinGecko
`simple/price` endpoint every 2 minutes, then publishes on the BoxTalk service
`cryptoTicker`:

```xml
<notify uuid="crypto-c-3f7a" serviceid="urn:hcb-hae-com:serviceId:cryptoTicker">
  <price>EUR 58.2k</price>
  <sub>BTC  +1.2% 24h</sub>
  <alert>BTC &lt;= EUR 50k (nu EUR 48.2k)</alert>
</notify>
```

`price`/`sub` drive the tile; a non-empty `alert` is surfaced by toonui as a
notification (the generic `alert_field` channel) and cleared when it goes empty.

See [`../../docs/`](../../docs) for the BoxTalk protocol and the integration
guide.

## Build

```sh
make            # cross-compile → ./crypto (ARMv7 hardfloat)
make tarball    # package crypto + manifest + README + conf for the catalog
```
