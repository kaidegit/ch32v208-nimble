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

# Request 2M for both directions, verify the negotiated PHY, then write/read.
tools/macos-ble/ble.sh probe --phy 2m --write-hex '00 11 22 33' --timeout 30

# Switch PHY without disconnecting; run the data operations at each rate.
tools/macos-ble/ble.sh probe --phy 1m,2m --write-hex '00 ff 80 7f' --timeout 40

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

PHY tests require the new `…3e04` characteristic. Write a one-byte mask (`01`
for 1M or `02` for 2M). Its six-byte status is `[version=1, HCI TX PHY, HCI RX PHY,
pending, last update status, requested mask]`; HCI PHY values 1/2 mean 1M/2M.
The firmware uses NimBLE's HCI Set PHY / Read PHY commands and reports the
PHY Update Complete event. The client allows one second for initial procedures
to settle, then waits up to eight seconds per transition,
checks both directions before and after GATT data operations, and fails if the
requested rate was refused or a transition timed out. It does not infer link
speed from elapsed GATT time. Legacy advertisements remain on 1M.

macOS may cache the old GATT database for this board's public address and omit
`…3e04` even after reflashing. The firmware has a separate test identity option:

```sh
cmake -S . -B build -DCH32_BLE_TEST_IDENTITY=ON
cmake --build build
scripts/run.sh --verify
```

This uses a deterministic static random address derived from the public address,
with no persistent writes. The name stays `CH32V208-NIMBLE`; CoreBluetooth assigns
a different identifier to that identity. The option defaults to OFF; use
`-DCH32_BLE_TEST_IDENTITY=OFF` to restore public-address advertising. Its purpose
is fresh service discovery during schema changes, not PHY selection. This
workaround is not evidence that the public-address firmware lacks the new service.
Coded PHY is disabled and is not accepted by `--phy`.

## Reliability runs

One continuous connection at a fixed PHY (duration begins after PHY confirmation):

```sh
tools/macos-ble/ble.sh probe --phy 1m --duration 1800 --timeout 1830
```

Without `--write-hex`, each round uses a changing payload, cycling lengths
0,1,2,3,4,5,7,16,20,23,31,32,63,64. Every round reads uptime, writes echo,
checks exact readback and confirms the PHY; uptime must never move backwards.
A read/write error, mismatch, unexpected disconnect or ten seconds without
callback progress fails immediately. `--interval` is the pause between rounds
(default 0.2 seconds); it is not a per-operation timeout. `--write-hex` can select
a fixed payload instead. Completion requires a whole verified round at or after
`--duration`; a global timeout is a failure, never a successful soak result.
The final summary includes completed rounds, verified bytes, elapsed soak time
and the maximum observed round duration.

The Python wrapper runs 30 resets, 100 reconnects (alternating 1M/2M) and a separate
30-minute continuous connection at each PHY. It does not require Python packages:

```sh
python3 tools/macos-ble/reliability.py

# Run an individual phase or adjust the run length.
python3 tools/macos-ble/reliability.py --phase resets --resets 30
python3 tools/macos-ble/reliability.py --phase reconnect --reconnects 100
python3 tools/macos-ble/reliability.py --phase soak --duration 1800
python3 tools/macos-ble/reliability.py --phase soak --phys 2m --duration 1200
```

Results are saved under `.build/reliability-<UTC timestamp>/`: `report.json`
records each case, firmware ELF hash, revision, working changes and client source
hash (and the runner source hash in current versions); raw JSONL logs preserve every callback. Each reset/reconnect case writes
and verifies one payload (lengths 0,1,3,20,63,64). This tests software reset through
probe-rs, not a physical power cycle. The wrapper stops on the first failure;
there is no automatic retry that can turn a failed case into a pass.

After a failure it uses probe-rs GDB plus `/tmp/gdb-wch-fixed` (override with
`--gdb`) to save registers/diagnostics and 64KB RAM. The core stays halted to
preserve evidence; finish diagnosis and reset before the next run. GDB is never
attached while a successful continuous-link test is running. Use `--id` to select
the exact board if multiple devices use the same name. The wrapper uses `caffeinate` while it runs to prevent idle sleep, without changing
Bluetooth or power settings. Only one process should control the probe/client
during a run.
