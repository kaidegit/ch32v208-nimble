# macOS BLE test client

Native Swift / CoreBluetooth command-line client for the CH32V208 NimBLE test
firmware. Requires macOS with Bluetooth and the Xcode Command Line Tools. It has
no Python packages or third-party dependencies. The wrapper builds a local
binary, embeds the Bluetooth usage descriptions, and applies an ad hoc signature
with the stable identifier `local.wch.macos-ble`.

Run from the repository root:

```sh
# Verify CLI parsing, exact filters, hex parsing, and little-endian uptime decoding.
tools/macos-ble/ble.sh --self-test

# Scan all devices to check that the Mac receives nearby BLE advertisements.
tools/macos-ble/ble.sh scan --timeout 15

# Still log all devices, but mark exact target matches with "matched":true.
tools/macos-ble/ble.sh scan --name CH32V208-NIMBLE --timeout 30 \
  > tools/macos-ble/.build/scan.jsonl

# Connect only to CH32V208-NIMBLE; discover all services/characteristics,
# then read the test uptime and echo values.
tools/macos-ble/ble.sh probe --timeout 30

# Explicit opt-in: write up to 64 bytes to echo, then verify the readback.
tools/macos-ble/ble.sh probe --name CH32V208-NIMBLE --write-hex '00 11 22 33' --timeout 30

# Select an exact CoreBluetooth identifier from an advertisement event.
tools/macos-ble/ble.sh probe --id XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX --timeout 30
```

`--name` matches the full advertised local name or full CoreBluetooth cached
peripheral name, with case sensitivity. When both `--name` and `--id` are passed,
both must match. `probe` defaults to the exact name `CH32V208-NIMBLE` only when
neither filter is supplied, stops scanning at its first match, and never connects
to other discovered devices. `scan` only observes advertisements. `--duplicates`
requests repeated advertisement callbacks; macOS may still coalesce reports.

Output is JSONL, with UTC timestamps and a final `summary` event. Advertisement
events include the CoreBluetooth UUID, local/cached names, RSSI, match status,
and all advertisement fields exposed by CoreBluetooth. Binary fields are encoded
as hex. When reflashing the same public MAC, macOS can retain the previous firmware's
name, including `kCBAdvDataLocalName`. During this investigation the NimBLE
manufacturer bytes `5743` appeared under the previous `Simple Peripheral` name.
Compare the stable CoreBluetooth identifier as well as the name; use `--id` for
a known board when diagnosing name caching. An empty name match count alone
does not prove that the board was absent from an unfiltered scan.

macOS does not expose BLE MAC addresses, raw radio packets, advertising
channel, or CRC validity through CoreBluetooth. A scan receiving other devices
but no target shows that the Mac's scan path works; it does not by itself identify
why the board's advertisements are absent. Stored logs may contain nearby device
identifiers and names; `.build/` and `*.jsonl` here are ignored by Git.

The probe expects service `7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e01`, uptime
`…3e02` (4-byte little-endian seconds), and echo `…3e03` (up to 64 bytes). The
UUIDs match `app/app_ble.c`. Reads and optional writes are serialized. A probe
failure, missing test characteristic, malformed uptime, or echo mismatch returns
a nonzero exit code. The global timeout includes Bluetooth startup, scanning,
connection, discovery, and GATT operations. On completion the client keeps its
run loop alive for the asynchronous disconnect, with an additional two-second
cleanup bound, before exiting. This makes repeated probe runs wait for the
previous connection to close.

On the first run, macOS may request Bluetooth permission for the binary or its
launching app. Grant access in the system prompt. If denied, enable the relevant
entry under **System Settings → Privacy & Security → Bluetooth** and rerun. The
tool reports unauthorized, powered-off, and unsupported states explicitly and
does not change system Bluetooth settings. Rebuilding changes the ad hoc signed
binary and macOS may ask for permission again. There is no pairing or bonding
operation in this client.

Exit codes: `0` completed scan or successful probe; `2` invalid arguments or
self-test failure; `3` Bluetooth unavailable/unauthorized/startup timeout; `4`
target not found or probe timeout; `5` connection/GATT/test failure; `130` Ctrl-C.
A completed scan may have zero target matches; inspect `matched_devices` in the
summary. Use `probe` when target absence should yield a nonzero exit status.
