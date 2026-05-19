# hello-solar

Minimal freetoon-lvgl integration. Publishes a fake solar reading every 10 s. Use as a copy-paste starting point.

## What it does

- Connects to BoxTalk on `127.0.0.1:1337`
- Announces itself with `ssdp:alive` + service id `solarProduction`
- Every 10 s, emits `<notify><power_w>N</power_w><today_kwh>M</today_kwh></notify>`
- Re-announces every 60 s so toonui can re-subscribe after a restart

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/Ierlandfan/freetoon-integrations/main/install.sh | sh -s -- hello-solar
```

Or manually:

```sh
mkdir -p /mnt/data/integrations/hello-solar
cd /mnt/data/integrations/hello-solar
# (download hello-solar.sh + manifest.json into this folder)
chmod +x hello-solar.sh
echo 'hsol:345:respawn:/mnt/data/integrations/hello-solar/hello-solar.sh >> /var/volatile/tmp/integration-hello-solar.log 2>&1' >> /etc/inittab
kill -HUP 1
```

## Building your own from this template

1. Copy this directory to a new name.
2. Edit `manifest.json` — change `id`, `service_id`, tile colour, icon name (sun/cloud/drop/fan/flame/radiator/leaf).
3. Replace the body of `hello-solar.sh` (or rewrite in any language) with your data source. Keep the BoxTalk announce + notify shape.
4. Test on a real Toon. Confirm the home tile picks up your values.
5. Open a PR to add yourself to `catalog/index.json`.

## Wire format reminders

- Each BoxTalk message is **XML followed by a literal NUL byte** (`\0`).
- The hub does no schema validation — fields you make up will get through. toonui parses by element name, so match your manifest's `value_field` / `subtitle_field`.
- `uuid` must be unique per integration instance (a constant per integration is fine).
- `serviceid` matches `urn:hcb-hae-com:serviceId:<your-service-id>` from the manifest.
