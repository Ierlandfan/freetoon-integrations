# freetoon-integrations

Marketplace catalog + example integrations for [freetoon-lvgl](https://github.com/Ierlandfan/freetoon-lvgl).

## What's an integration?

A standalone **C daemon** that runs alongside `toonui` on the Toon and publishes its data over the local **BoxTalk** bus (`127.0.0.1:1337`) as a registered service. toonui's BoxTalk client subscribes to known services and renders them as tiles on the home screen.

The pattern matches what `p1bridge` and `quby_bridge` already do for HomeWizard P1 / Quby — except those are baked into the toonui build. Anything you can write in C that talks to BoxTalk can become a tile.

## How does the user install one?

From toonui's **Settings → Marketplace** screen they pick from this repo's catalog. toonui fetches the integration's tarball, drops it in `/mnt/data/integrations/<name>/`, wires an inittab respawn row, and HUPs init.

Manual install from a shell on the Toon:

```sh
curl -fsSL https://raw.githubusercontent.com/Ierlandfan/freetoon-integrations/main/install.sh | sh -s -- hello-solar
```

## Catalog format

`catalog/index.json` lists everything toonui's Marketplace screen can offer. One entry per integration:

```json
{
  "id": "hello-solar",
  "name": "Hello Solar (template)",
  "description": "...",
  "author": "freetoon team",
  "version": "1.0.0",
  "url": ".../hello-solar.tar.gz",
  "manifest_url": ".../manifest.json",
  "tile": {
    "title": "Solar",
    "color": "0xffcc44",
    "icon": "sun",
    "service": "solarProduction"
  }
}
```

The `tile.service` field is the BoxTalk `serviceId` the integration registers; toonui subscribes to it after install. Phase 2 of the marketplace UI will let users assign any installed integration to any home tile slot.

## Per-integration layout

```
my-integration/
├── my-integration       ← cross-compiled ARMv7 binary
├── hello-solar.c        ← source (any C that builds with gcc-arm-linux-gnueabihf)
├── Makefile             ← cross-compile recipe
├── manifest.json        ← name, version, service-id, tile metadata
└── README.md            ← what it does, how to configure
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
    "icon": "drop",
    "value_field": "current",
    "value_unit": "L",
    "subtitle_field": "today_l",
    "subtitle_unit": "L today"
  }
}
```

## Writing your own

See [`examples/hello-solar/`](examples/hello-solar/) — the canonical C-language template. The building blocks:

1. Open TCP `127.0.0.1:1337`
2. Send `<discovery nts="ssdp:alive" uuid="…" type="…"><service type="myService" version="1"/></discovery>` + a NUL byte
3. Periodically send `<notify uuid="…" serviceid="urn:hcb-hae-com:serviceId:myService"><field>value</field></notify>` + NUL
4. Re-announce every 30–60 s so toonui can resubscribe after restarts

Full protocol details: [`docs/BOXTALK_PROTOCOL.md`](docs/BOXTALK_PROTOCOL.md).

## Why C-only?

Integrations are init-managed daemons running on a resource-constrained device. Shell scripts depend on busybox quirks (`printf '\0'`, `nc` buffering, signal handling) and have multiple known pitfalls — long-sleep-blocks-init-respawn, EPIPE doesn't reconnect, framing mistakes are silent. C eliminates that whole class of problem with a 4-5 KB stripped binary and no runtime dependencies.

If you absolutely need a scripting language (Python, Go, Rust), bring a statically-linked interpreter / runtime and ship it alongside your binary in the tarball.

## Submitting your integration

PRs welcome:

1. Fork this repo
2. Drop your integration under `examples/<your-name>/`
3. Add an entry to `catalog/index.json`
4. Test that `./install.sh <your-name>` works on a real Toon
5. Open a PR with screenshots of the resulting tile

## License

MIT — each integration may have its own license; check the integration's directory.
