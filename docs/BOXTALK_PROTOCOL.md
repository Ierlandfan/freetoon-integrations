# BoxTalk wire-format quick reference

Everything you need to write a freetoon-lvgl integration that toonui will pick up as a tile.

## Connection

- TCP `127.0.0.1:1337` on the Toon.
- One persistent connection per integration; reconnect on close.
- Each message is **XML text followed by a single NUL byte** (`0x00`).
- No length prefix, no framing escape — just XML + NUL.

## Required messages

### 1. Announce yourself

Send this once at startup and re-send every 30–60 s so toonui can re-subscribe after a restart:

```xml
<discovery nts="ssdp:alive" uuid="YOUR-UUID-HERE"
           type="urn:schemas-hcb-hae-com:device:thirdParty"
           version="v"
           xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <service type="YOUR-SERVICE-NAME" version="1"/>
</discovery>
```

- `uuid`: any string unique to your integration. Pick one constant (e.g. `solar-12345`).
- `type`: cosmetic; toonui doesn't filter on it.
- `service type`: **must** match the `service_id` in your `manifest.json` so subscribers know what they're listening to.

### 2. Publish values

Whenever your sensor produces a new reading:

```xml
<notify uuid="YOUR-UUID-HERE"
        serviceid="urn:hcb-hae-com:serviceId:YOUR-SERVICE-NAME">
  <power_w>1234</power_w>
  <today_kwh>8.3</today_kwh>
</notify>
```

- Element names inside the notify are arbitrary. Whatever you name them, list them in your manifest's `tile.value_field` / `tile.subtitle_field` so toonui knows what to display.
- Numeric values: integers and floats both work; toonui's parser uses `atof()`.
- String values: enclose in the tag verbatim. Quotes/special chars aren't escaped here — assume HTML-safe ASCII.

## Optional messages

### Subscribe to another service

If your integration needs data from another integration (or from `happ_thermstat`), subscribe to it the same way toonui does:

```xml
<subscribe uuid="YOUR-UUID-HERE" destuuid="">
  <target uuid="" serviceid="urn:hcb-hae-com:serviceId:OtherService"/>
</subscribe>
```

You'll then receive `<notify>` frames from that service inline on the same socket.

### Query a current value

```xml
<query class="invoke" uuid="YOUR-UUID-HERE"
       destuuid="REMOTE-UUID"
       serviceid="urn:hcb-hae-com:serviceId:OtherService"
       requestid="MY-REQ-1">
  <u:QueryStateVariable xmlns:u="urn:hcb-hae-com:service:OtherService:1">
    <varName>SomeField</varName>
    <requestId>MY-REQ-1</requestId>
    <timeout>30</timeout>
  </u:QueryStateVariable>
</query>
```

The response comes back as a regular notify-shaped frame with `class="response"` and your `requestid` echoed.

## Don'ts

- Don't omit the trailing NUL — the hub buffers until it sees one, and your message is invisible until then.
- Don't send multiple messages in one buffer without NUL separators.
- Don't reuse a UUID belonging to a system daemon (`b822de89-…` = happ_thermstat, etc.).
- Don't spam notifies faster than ~1 Hz; the hub fans messages out to every subscriber.

## Debugging

`toonui` logs every received notify when running with stderr captured (`/var/volatile/tmp/toonui.log`). Look for lines like:

```
[bxt] msg: <notify uuid="…" serviceid="…
```

If your messages don't appear there, check:
- TCP connection actually established (`netstat -an | grep 1337`)
- NUL byte being sent (`hexdump` your output)
- XML well-formed (toonui's parser is strict on attribute quoting)
