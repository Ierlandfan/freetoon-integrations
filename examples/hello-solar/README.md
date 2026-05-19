# hello-solar

C-language template for a freetoon-lvgl integration. Cross-compile once, ship the binary.

## Why C?

The only supported integration language in this catalog is C — pre-built ARMv7 binaries with no runtime dependencies:

- **One static binary** — installs as a single file, no interpreter / nc / curl / jq dependency on the Toon
- **Deterministic timing** — `sleep(10)` from a daemon doesn't drift like a shell pipeline does
- **Real error handling** — `EPIPE` reconnects in-process instead of dying + waiting for init to respawn
- **Native data sources** — read sensors via sysfs, `mmap`, libudev, etc.
- **Sub-second polling** — a shell loop bottoms out around 1 Hz; C polls much faster if you need it

## Build

Needs the linaro armv7-hardfloat toolchain that ships with the freetoon-lvgl dev tree (`/tmp/qt_rebuild/linaro/`). If it's elsewhere on your machine, override `CROSS`:

```sh
make CROSS=/path/to/arm-linux-gnueabihf-
```

`make tarball` packages binary + manifest + README into `hello-solar.tar.gz` ready for the marketplace catalog to point at.

## Install via marketplace

Either through toonui's **Settings → Marketplace** screen, or manually on the Toon:

```sh
curl -fsSL https://raw.githubusercontent.com/Ierlandfan/freetoon-integrations/main/install.sh | sh -s -- hello-solar
```

That fetches `hello-solar.tar.gz` from this directory, drops it in `/mnt/data/integrations/hello-solar/`, adds an inittab respawn row, HUPs init. Logs land in `/var/volatile/tmp/integration-hello-solar.log`.

## Wire-format reminder

See [`docs/BOXTALK_PROTOCOL.md`](../../docs/BOXTALK_PROTOCOL.md) for the full spec. TL;DR:

1. `discovery ssdp:alive` once on connect + every ~60 s after
2. `notify` frames with your data fields
3. Each frame: XML + a single `0x00` byte terminator

The reference code uses `send(fd, xml, len, MSG_NOSIGNAL); send(fd, &nul, 1, MSG_NOSIGNAL);` per frame — two writes, one terminator. **Forgetting the NUL = silent message that never reaches subscribers.** Easy mistake; the hub buffers until it sees the terminator.

## Adapting to your sensor

Replace the body of the inner loop in `hello-solar.c`:

```c
int power_w   = (tick % 30) * 100;   /* fake — your sensor goes here */
int today_kwh = (tick / 360) + 5;
```

Real-data examples:

- **Read sysfs**: `FILE* f = fopen("/sys/class/.../in0_input", "r"); fscanf(f, "%d", &v); fclose(f);`
- **HTTP call**: `popen("curl -s http://device/api/data", "r")` and parse the line. Or link libcurl statically if you want better error handling.
- **i2c hardware**: open `/dev/i2c-1`, `ioctl(I2C_SLAVE, addr)`, `read()`.
- **UART hardware**: open `/dev/ttymxc1`, `tcsetattr` for baud + raw mode, `read()` your protocol's frames.

The rest of the scaffold (announce, notify, reconnect on `EPIPE`) stays unchanged. Submit a PR adding your integration to `catalog/index.json` when it's working.
