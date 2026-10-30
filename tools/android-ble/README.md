# Android phone BLE diagnostics over ADB

The host Python script operates the **installed nRF Connect** scanner through
ADB. It installs no APK, creates no Android application, and never changes the
user's name/RSSI/manufacturer filters or Bluetooth settings.

Requirements: Python 3, authorized USB debugging, nRF Connect
(`no.nordicsemi.android.mcp`), an unlocked phone, JDK 8+ and an Android SDK with
platform, UIAutomator and build tools. These are already available on this Mac.
Set `ADB` / `ANDROID_HOME` or use `--adb` / `--sdk` elsewhere. Multiple attached
phones require an explicit `--serial`.

```sh
# Read phone/permission/Bluetooth diagnostics and preserve the current screen.
python3 tools/android-ble/phone_ble.py inspect

# Start scanning, observe for 20 seconds, pause, and collect results.
python3 tools/android-ble/phone_ble.py scan --duration 20

# Request a connection to exactly one visible matching target.
python3 tools/android-ble/phone_ble.py connect \
  --address D0:54:7B:21:AE:B1

# On the connected target tab: read uptime, write/read echo, request 2M.
python3 tools/android-ble/phone_ble.py gatt --address D0:54:7B:21:AE:B1
python3 tools/android-ble/phone_ble.py gatt --address D0:54:7B:21:AE:B1 \
  --uuid 7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e03 --write-hex '00 ff 80 7f'
python3 tools/android-ble/phone_ble.py gatt --address D0:54:7B:21:AE:B1 \
  --uuid 7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e04 --write-hex 02
python3 tools/android-ble/phone_ble.py disconnect --address D0:54:7B:21:AE:B1 --close
```

`--name` (default `CH32V208-NIMBLE`) and optional `--address` are host result
selectors; they do **not** edit the phone's filters. A target excluded by the
phone's current filters may be absent from its visible list. The script saves
the observed filter header and exclusions in its report.

Outputs go to ignored `.build/phone-<timestamp>/`, or a new `--output` directory:
fresh UI XML, active/paused scanner screenshots, scoped nRF logcat, Bluetooth
dumps and `report.json`. UIAutomator exports use unique filenames and are
validated so a failed export cannot silently reuse old rows. The scanner is
paused during cleanup; no unrelated device is connected or paired.
`--close` closes only the verified target tab after disconnecting, then restores
the scanner tab. The app's filters remain as configured by the user.

A small temporary **shell test JAR**, built from `ShellUi.java`, is pushed to
`/data/local/tmp/wch-shell-ui.jar` and run with Android's existing UIAutomator
framework. It exports the UI hierarchy and can type a hex payload only into
nRF's **Write value** dialog. Paced key events are necessary: accessibility
`ACTION_SET_TEXT` changed the displayed text but nRF sent an empty value in
this version; the tool therefore verifies actual GATT readback. This also avoids the normal
`uiautomator dump` idle-state failure on continuously changing scan screens;
there is no package installation, launcher icon, service or persistent agent.
The idle timeout is process-local. Remove the helper with
`adb shell rm /data/local/tmp/wch-shell-ui.jar` when no longer needed.

The script also decodes Android's existing in-memory `BTSNOOP_LOG_SUMMARY`
without enabling HCI logging. `hci-summary.json` reports target advertising and
scan-response counts and RSSI, independently of the nRF UI. A baseline ring
timestamp excludes packets already present before this invocation. A completed
scan with HCI available requires a new target report as well as a visible UI
row, so an old retained row alone cannot pass. The ring has finite capacity;
very busy traffic can discard older packets. Logs can contain nearby
identifiers, so raw files stay ignored.

Connection success requires `CONNECTED` and the test service UUID in the UI.
GATT operations require the exact selected target address, and only expose
the three test characteristics: uptime (read), echo (read/write, up to64 bytes),
and PHY (write01/02, read negotiated status). Echo writes must match readback;
PHY writes must finish with both actual directions matching the request.
Failed actions are not retried automatically; the saved UI helps diagnosis.
The dump helper may repeat only the UI read briefly during a window transition;
it never repeats the payload input because of a missing XML file.

Offline decoder/UI-boundary checks:

```sh
python3 -m unittest discover -s tools/android-ble -p 'test_*.py'
```

The btsnooz format follows the [AOSP decoder](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/refs/heads/main/system/tools/scripts/btsnooz.py).
Android's [advertising processing](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/cdb1c6dca4/system/stack/btm/btm_ble_gap.cc)
can hold a scannable advertisement during active scanning until its scan
response arrives. HCI primary reports without a visible nRF row therefore do
not by themselves imply an application filter problem.

Exit codes: `0` completed operation, `4` no fresh visible target,
`5` connection/discovery/disconnection failure, `6` GATT/PHY verification
failure, `2` tool/ADB/UI error. Connection requests alone do not count as success.
