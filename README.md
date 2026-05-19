# freetoon-integrations

Marketplace catalog + example sidecars for [freetoon-lvgl](https://github.com/Ierlandfan/freetoon-lvgl).

## What's an integration?

A freetoon-lvgl integration is a **standalone sidecar daemon** that runs alongside `toonui` on the Toon and publishes its data over the local **BoxTalk** bus (`127.0.0.1:1337`) as a registered service. toonui's BoxTalk client subscribes to known services and renders them as tiles on the home screen.

The pattern matches what `p1bridge` and `quby_bridge` already do for HomeWizard P1 / Quby — except those are baked-in. Anything you can write that talks to BoxTalk can become a tile.

## How does the user install one?

From toonui's **Settings → Marketplace** screen they pick from this repo's catalog. toonui fetches the integration's tarball, drops it in `/mnt/data/integrations/<name>/`, wires an inittab respawn row, and HUPs init.

Manual install from a shell on the Toon:

```sh
curl -fsSL https://raw.githubusercontent.com/Ierlandfan/freetoon-integrations/main/install.sh | sh -s -- hello-solar
```

## Catalog format

`catalog/index.json` lists everything toonui can offer in its Marketplace screen. One entry per integration:

```json
{
  "id": "hello-solar",
  "name": "Hello Solar",
  "description": "Example tile that emits a fake solar-production value every 10 seconds. Use as a starting point.",
  "author": "freetoon team",
  "version": "1.0.0",
  "url": "https://github.com/Ierlandfan/freetoon-integrations/raw/main/examples/hello-solar/hello-solar.tar.gz",
  "manifest_url": "https://raw.githubusercontent.com/Ierlandfan/freetoon-integrations/main/examples/hello-solar/manifest.json",
  "tile": {
    "title": "Solar",
    "color": "0xffcc44",
    "icon": "sun",
    "service": "solarProduction"
  }
}
```

The `tile.service` field is the BoxTalk `serviceId` the integration registers; toonui subscribes to it after install.

## Per-integration layout

Each integration directory under `examples/` (or anywhere a user hosts one) ships:

```
my-integration/
├── manifest.json     # name, version, service-id, binary name, inittab args
├── my-integration    # the executable (any language; statically-linked or
│                     # interpreted via an interpreter that's on the Toon)
├── README.md         # what it does, how to configure
└── (optional) icon.png
```

`manifest.json`:

```json
{
  "id": "my-integration",
  "name": "My Integration",
  "version": "1.0.0",
  "binary": "my-integration",
  "args": [],
  "service_id": "myService",
  "tile": {
    "title": "My Sensor",
    "color": "0x66ddaa",
    "icon": "drop"
  }
}
```

## Writing your own

See [`examples/hello-solar/`](examples/hello-solar/) for a minimal POSIX-shell integration that publishes a fake reading every 10 s. The key building blocks:

1. Connect to `127.0.0.1:1337` over TCP.
2. Send an `<discovery nts="ssdp:alive" uuid="…" type="…">…</discovery>` announce frame (XML, NUL-terminated).
3. Periodically send `<notify uuid="…" serviceid="urn:hcb-hae-com:serviceId:myService"><myField>VALUE</myField></notify>` frames with your data.
4. Re-announce every 30 s so toonui can resubscribe after restarts.

Full BoxTalk protocol details live in the main repo at [`docs/boxtalk.md`](https://github.com/Ierlandfan/freetoon-lvgl/blob/main/lvgl_ui_recovered/src/boxtalk.c) (header comments) and the RE notes at `/tmp/qt_rebuild/boxtalk_re/`.

## Submitting your integration

PRs welcome. To add a new integration to the catalog:

1. Fork this repo
2. Drop your integration under `examples/<your-name>/`
3. Add an entry to `catalog/index.json`
4. Test that `./install.sh <your-name>` works on a real Toon
5. Open a PR

## License

MIT — each integration may have its own license; check that integration's directory.
