# hello-solar-c

C-language template for a freetoon-lvgl integration. Cross-compile once, ship the binary. No shell, no nc, no curl on the device.

## Why C over shell?

The shell variant (`../hello-solar/`) is short and obvious, but it depends on `nc`, `printf` with `\0`, and busybox's particular quirks. The C version is more code but gives you:

- **One static binary** — installs as a single file, no interpreter dependency
- **Deterministic timing** — `sleep(10)` from a daemon doesn't drift like a shell pipeline does
- **Real error handling** — `EPIPE` reconnects in-process instead of dying + waiting for init to respawn
- **Native data sources** — read sensors via sysfs, `mmap`, libudev, etc.
- **Sub-second polling** — the shell loop bottoms out around 1 Hz; C can poll much faster

For most production integrations this is the recommended template.

## Build

You need the linaro armv7-hardfloat toolchain that ships with the
freetoon-lvgl dev tree (`/tmp/qt_rebuild/linaro/`). If it's somewhere
else, override the `CROSS` variable:

```sh
make CROSS=/path/to/arm-linux-gnueabihf-
```

`make tarball` packages binary + manifest + README into `hello-solar-c.tar.gz` ready for the marketplace catalog.

## Install via marketplace

Either through toonui's **Settings → Marketplace** screen, or manually on the Toon:

```sh
curl -fsSL https://raw.githubusercontent.com/Ierlandfan/freetoon-integrations/main/install.sh \
  | sh -s -- hello-solar-c
```

That fetches `hello-solar-c.tar.gz` from this directory, drops it in `/mnt/data/integrations/hello-solar-c/`, and adds an inittab respawn row. Logs land in `/var/volatile/tmp/integration-hello-solar-c.log`.

## Wire-format reminder

Same as the shell version — see [`docs/BOXTALK_PROTOCOL.md`](../../docs/BOXTALK_PROTOCOL.md):

1. `discovery ssdp:alive` once on connect + every ~60 s
2. `notify` frames with your data fields
3. Each frame: XML + a single `0x00` byte terminator

The C code uses `send(fd, xml, len, MSG_NOSIGNAL); send(fd, &nul, 1, MSG_NOSIGNAL);` per frame — two writes, one terminator. Forgetting the NUL = silent message.

## Adapting to your sensor

Replace the body of the inner loop:

```c
int power_w   = (tick % 30) * 100;   /* fake — your sensor goes here */
int today_kwh = (tick / 360) + 5;
```

with whatever produces your real reading. Examples:

- Read sysfs: `FILE* f = fopen("/sys/class/.../in0_input", "r"); fscanf(f, "%d", &v); fclose(f);`
- HTTP call: `popen("curl -s http://device/api", "r")` and parse the line
- Hardware via i2c: open `/dev/i2c-1`, `ioctl(I2C_SLAVE)`, `read()`

The rest of the integration scaffold (announce, notify, reconnect on `EPIPE`) stays unchanged.
