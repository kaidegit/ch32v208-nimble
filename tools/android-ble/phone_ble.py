#!/usr/bin/env python3
"""Control the installed nRF Connect scanner over ADB; no APK is installed."""
import argparse
import base64
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import struct
import sys
import time
import xml.etree.ElementTree as ET
import zipfile
import zlib

HERE = Path(__file__).resolve().parent
PKG = 'no.nordicsemi.android.mcp'
SERVICE = '7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e01'


def emit(event, **fields):
    print(json.dumps(dict(event=event, time=dt.datetime.now(dt.timezone.utc).isoformat(),
                          **fields), ensure_ascii=False), flush=True)


def adb_path(explicit):
    candidates = [explicit, os.environ.get('ADB'), shutil.which('adb')]
    for env in ('ANDROID_HOME', 'ANDROID_SDK_ROOT'):
        if os.environ.get(env):
            candidates.append(str(Path(os.environ[env]) / 'platform-tools/adb'))
    candidates += [str(Path.home() / 'Library/Android/sdk/platform-tools/adb'),
                   '/Volumes/aigo_1t/DevPkgs/android/platform-tools/adb',
                   str(HERE / '.build/platform-tools/adb')]
    for candidate in candidates:
        if candidate and Path(candidate).is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise RuntimeError('ADB not found; set ADB or pass --adb /path/to/adb')


def find(root, ident):
    return next((n for n in root.iter('node')
                 if n.get('resource-id') == PKG + ':id/' + ident), None)


def rows(root):
    result = []
    for row in root.iter('node'):
        if row.get('resource-id') != PKG + ':id/device_main':
            continue
        fields = {}
        for key in ('display_name', 'address', 'rssi', 'adv_interval'):
            n = find(row, key)
            fields[key] = n.get('text', '') if n is not None else ''
        result.append((fields, row))
    return result


def characteristic(root, uuid):
    parents = {child: parent for parent in root.iter() for child in parent}
    node = next((n for n in root.iter('node') if n.get('resource-id') == PKG + ':id/uuid'
                 and uuid.lower() in n.get('text', '').lower()), None)
    while node is not None and find(node, 'action_read') is None:
        node = parents.get(node)
    if node is None:
        raise RuntimeError('Test characteristic not visible; see UI XML')
    return node


def value_bytes(root, uuid):
    found = False
    for node in root.iter('node'):
        ident, text = node.get('resource-id'), node.get('text', '')
        if ident == PKG + ':id/uuid':
            if found:
                break
            found = uuid.lower() in text.lower()
        if found and ident == PKG + ':id/value':
            if text.strip() == 'Value:':
                return b''
            match = re.search(r'\(0x\)\s*([0-9a-fA-F-]*)', text)
            if match:
                return bytes.fromhex(match[1].replace('-', ''))
    raise RuntimeError('Hex characteristic value not present after read')


def hci_summary(dump, name, address, since_timestamp=None):
    """Decode Android's in-memory btsnooz ring; no logging settings change.

    Format: AOSP packages/modules/Bluetooth/system/tools/scripts/btsnooz.py.
    With a baseline timestamp, exclude packets already present at baseline.
    """
    match = re.search(r'--- BEGIN:BTSNOOP_LOG_SUMMARY[^\n]*\n(.*?)'
                      r'--- END:BTSNOOP_LOG_SUMMARY', dump, re.S)
    if not match:
        return dict(available=False)
    encoded = base64.b64decode(match[1])
    version = encoded[0]
    if version not in (1, 2):
        return dict(available=False, reason=f'unsupported btsnooz version {version}')
    data = zlib.decompress(encoded[9:])
    offset, records, entries = 0, 0, []
    devices = {}
    while offset < len(data):
        if version == 2:
            length, original, delta, kind = struct.unpack_from('<HHIB', data, offset)
            offset += 9
        else:
            length, delta, kind = struct.unpack_from('<HIB', data, offset)
            offset += 7
            original = length
        packet = data[offset:offset + length - 1]
        offset += length - 1
        records += 1
        entries.append((delta, kind, packet))
    timestamp = struct.unpack_from('<Q', encoded, 1)[0] - sum(e[0] for e in entries)
    for delta, kind, packet in entries:
        timestamp += delta
        if since_timestamp is not None and timestamp <= since_timestamp:
            continue
        if kind != 0x10 or len(packet) < 4 or packet[0] != 0x3e:
            continue
        subevent, count, pos = packet[2], packet[3], 4
        if subevent not in (2, 13):
            continue
        for _ in range(count):
            if subevent == 13:
                if pos + 24 > len(packet):
                    break
                event_type = struct.unpack_from('<H', packet, pos)[0]
                mac = packet[pos + 3:pos + 9][::-1].hex(':').upper()
                rssi = struct.unpack_from('b', packet, pos + 13)[0]
                size = packet[pos + 23]
                payload = packet[pos + 24:pos + 24 + size]
                response = bool(event_type & 8)
                pos += 24 + size
            else:
                if pos + 9 > len(packet):
                    break
                event_type = packet[pos]
                mac = packet[pos + 2:pos + 8][::-1].hex(':').upper()
                size = packet[pos + 8]
                if pos + 10 + size > len(packet):
                    break
                payload = packet[pos + 9:pos + 9 + size]
                rssi = struct.unpack_from('b', packet, pos + 9 + size)[0]
                response = event_type == 4
                pos += 10 + size
            d = devices.setdefault(mac, dict(address=mac, names=[], primary_reports=0,
                                             scan_response_reports=0, rssi_min=127, rssi_max=-128,
                                             event_types={}, last_payload_hex='',
                                             scan_response_payloads={}))
            d['scan_response_reports' if response else 'primary_reports'] += 1
            d['rssi_min'], d['rssi_max'] = min(d['rssi_min'], rssi), max(d['rssi_max'], rssi)
            key = f'0x{event_type:04x}'
            d['event_types'][key] = d['event_types'].get(key, 0) + 1
            d['last_payload_hex'] = payload.hex()
            if response:
                key = payload.hex()
                d['scan_response_payloads'][key] = d['scan_response_payloads'].get(key, 0) + 1
            adv_pos = 0
            while adv_pos < len(payload):
                field_size = payload[adv_pos]
                if field_size == 0 or adv_pos + field_size >= len(payload):
                    break
                if payload[adv_pos + 1] in (8, 9):
                    advertised_name = payload[adv_pos + 2:adv_pos + 1 + field_size].decode(errors='replace')
                    if advertised_name not in d['names']:
                        d['names'].append(advertised_name)
                adv_pos += 1 + field_size
    targets = [d for mac, d in devices.items() if
               (mac == address.upper() if address else name in d['names'])]
    return dict(available=True, version=version, records=records, targets=targets,
                last_timestamp_us=struct.unpack_from('<Q', encoded, 1)[0],
                since_timestamp_us=since_timestamp,
                scope='packets newer than initial phone snapshot' if since_timestamp is not None
                      else 'in-memory ring, may include previous scans')


def build_helper(sdk):
    """Build a shell-only test JAR to dump a busy scanner without idle waits."""
    sdk = Path(sdk or os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or
               '/Volumes/aigo_1t/DevPkgs/android')
    platforms = sorted(sdk.glob('platforms/android-*/uiautomator.jar'))
    d8s = sorted(sdk.glob('build-tools/*/d8'))
    if not platforms or not d8s:
        raise RuntimeError('Android SDK platform/build-tools needed; set --sdk or ANDROID_HOME')
    platform = platforms[0].parent
    source = HERE / 'ShellUi.java'
    build = HERE / '.build/shell-ui'
    build.mkdir(parents=True, exist_ok=True)
    jar = build / 'shell-ui.jar'
    fingerprint = hashlib.sha256(b'shell-helper-v2' + source.read_bytes() + str(platform).encode()).hexdigest()
    stamp = build / 'source.sha256'
    if not jar.exists() or not stamp.exists() or stamp.read_text() != fingerprint:
        classes, dex = build / 'classes', build / 'dex'
        classes.mkdir(exist_ok=True)
        dex.mkdir(exist_ok=True)
        for old_class in classes.glob('*.class'):
            old_class.unlink()
        for old_dex in dex.glob('*.dex'):
            old_dex.unlink()
        libs = [platform / 'android.jar', platform / 'uiautomator.jar',
                platform / 'optional/android.test.base.jar']
        def run_build(command):
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError('Shell helper build failed: ' + result.stderr)
        run_build(['javac', '-source', '8', '-target', '8', '-cp', os.pathsep.join(map(str, libs)),
                   '-d', str(classes), str(source)])
        run_build([str(d8s[0]), '--min-api', '23', '--lib', str(libs[0]),
                   '--lib', str(libs[1]), '--lib', str(libs[2]),
                   '--output', str(dex), *map(str, classes.glob('*.class'))])
        with zipfile.ZipFile(jar, 'w', zipfile.ZIP_DEFLATED) as z:
            z.write(dex / 'classes.dex', 'classes.dex')
        stamp.write_text(fingerprint)
    return jar


class Phone:
    def __init__(self, adb, serial, output, helper):
        self.adb, self.serial, self.output = adb, serial, output
        self.seq = 0
        self.run('push', str(helper), '/data/local/tmp/wch-shell-ui.jar')

    def run(self, *args, binary=False, timeout=25):
        p = subprocess.run([self.adb, '-s', self.serial, *args],
                           capture_output=True, timeout=timeout)
        if p.returncode:
            raise RuntimeError(f'ADB {args[0]} failed: {p.stderr.decode(errors="replace").strip()}')
        return p.stdout if binary else p.stdout.decode(errors='replace')

    def ui(self, label, hex_value=None):
        self.seq += 1
        filename = f'wch-nrf-{os.getpid()}-{self.seq}.xml'
        remote = '/data/local/tmp/' + filename
        command = ['env', 'CLASSPATH=/system/framework/android.test.base.jar:'
                   '/system/framework/android.test.runner.jar:/system/framework/uiautomator.jar:'
                   '/data/local/tmp/wch-shell-ui.jar', 'app_process', '/system/bin',
                   'com.android.commands.uiautomator.Launcher', 'runtest', '-c', 'ShellUi',
                   '-e', 'output', filename]
        if hex_value is not None:
            command += ['-e', 'hex', hex_value]
        dumped = self.run('shell', shlex.join(command), timeout=60 if hex_value is not None else 25)
        if 'OK (1 test)' not in dumped or 'unexpected exception' in dumped:
            raise RuntimeError('UI helper failed (no stale XML accepted): ' + dumped.strip())
        xml = self.run('exec-out', 'cat', remote)
        self.run('shell', 'rm', remote)
        (self.output / f'{self.seq:03d}-{label}.xml').write_text(xml)
        root = ET.fromstring(xml)
        if not any(n.get('package') == PKG for n in root.iter('node')):
            raise RuntimeError('nRF Connect UI is not accessible; unlock the phone and open its scanner')
        return root

    def tap(self, node):
        if node is None or node.get('clickable') != 'true' or node.get('enabled') != 'true':
            raise RuntimeError('Expected enabled clickable nRF Connect control not found; see XML')
        if node.get('content-desc') != 'Scanner' and node.get('resource-id', '').split('/')[-1] not in (
                'action_scan_start', 'action_scan_stop', 'action_connect', 'action_disconnect', 'action_close'):
            if node.get('resource-id') not in [PKG + ':id/' + name for name in
                    ('service_main', 'action_read', 'action_write', 'action_add')] + ['android:id/button1']:
                raise RuntimeError('Refusing a control outside scanner/connection/test GATT actions')
        m = re.fullmatch(r'\[(\d+),(\d+)\]\[(\d+),(\d+)\]', node.get('bounds', ''))
        if not m:
            raise RuntimeError('Invalid control bounds')
        x1, y1, x2, y2 = map(int, m.groups())
        self.run('shell', 'input', 'tap', str((x1 + x2) // 2), str((y1 + y2) // 2))
        time.sleep(0.4)

    def screenshot(self, label):
        (self.output / f'{label}.png').write_bytes(self.run('exec-out', 'screencap', '-p', binary=True))

    def inspect(self):
        for name, args in {
            'package.txt': ('shell', 'dumpsys', 'package', PKG),
            'bluetooth.txt': ('shell', 'dumpsys', 'bluetooth_manager'),
        }.items():
            (self.output / name).write_text(self.run(*args))
        return {key: self.run('shell', 'getprop', prop).strip() for key, prop in
                [('model', 'ro.product.model'), ('android', 'ro.build.version.release'),
                 ('sdk', 'ro.build.version.sdk')]}

    def logs(self):
        pid = self.run('shell', 'pidof', PKG).strip()
        if re.fullmatch(r'\d+', pid):
            (self.output / 'nrf-logcat.txt').write_text(
                self.run('logcat', '-d', '-t', '1500', '--pid=' + pid))

    def hci(self, name, address, since_timestamp=None):
        dump = self.run('shell', 'dumpsys', 'bluetooth_manager')
        (self.output / 'bluetooth-after.txt').write_text(dump)
        result = hci_summary(dump, name, address, since_timestamp)
        (self.output / 'hci-summary.json').write_text(json.dumps(result, indent=2) + '\n')
        return result

    def connection(self, root, address, report, disconnect=False, close=False, requested=False):
        selected = [n.get('text', '').upper() for n in root.iter('node')
                    if n.get('resource-id') == 'android:id/text2' and n.get('selected') == 'true']
        if selected != [address.upper()]:
            raise RuntimeError('Selected nRF device tab does not match the exact target address')
        state = find(root, 'connection_state')
        if state is None:
            raise RuntimeError('Target connection panel missing')
        report['success'] = False
        if disconnect:
            if state.get('text') != 'DISCONNECTED':
                self.tap(find(root, 'action_disconnect'))
        elif state.get('text') == 'DISCONNECTED' and not requested:
            self.tap(find(root, 'action_connect'))
            requested = True
        report['connection_requested'] = requested
        deadline = time.monotonic() + 20
        while True:
            time.sleep(0.5)
            root = self.ui('connection-state')
            state = find(root, 'connection_state')
            status = state.get('text', '') if state is not None else ''
            report['connection_state'] = status
            text = ' '.join(n.get('text', '') for n in root.iter('node')).lower()
            report['service_discovered'] = SERVICE in text
            if disconnect and status == 'DISCONNECTED':
                report['success'] = True
                break
            if not disconnect and status == 'CONNECTED' and report['service_discovered']:
                report['success'] = True
                break
            if status == 'DISCONNECTED' or time.monotonic() >= deadline:
                break
        if disconnect and close and report['success']:
            parents = {child: parent for parent in root.iter() for child in parent}
            tab = next(n for n in root.iter('node') if n.get('resource-id') == 'android:id/text2'
                       and n.get('text', '').upper() == address.upper() and n.get('selected') == 'true')
            while tab is not None and find(tab, 'action_close') is None:
                tab = parents.get(tab)
            self.tap(find(tab, 'action_close') if tab is not None else None)
            root = self.ui('closed-target-tab')
            scanner = next((n for n in root.iter('node') if n.get('content-desc') == 'Scanner'), None)
            if scanner is not None and scanner.get('clickable') == 'true':
                self.tap(scanner)
                root = self.ui('scanner-restored')
            report['final_filters'] = {key: n.get('text') for key in ('filter_header', 'filter_excludes')
                                       if (n := find(root, key)) is not None}
        self.screenshot('connection-result')
        emit('connection_result', address=address, success=report['success'],
             state=report['connection_state'], service_discovered=report['service_discovered'])
        return 0 if report['success'] else 5

    def gatt(self, root, address, uuid, payload, report):
        selected = [n.get('text', '').upper() for n in root.iter('node')
                    if n.get('resource-id') == 'android:id/text2' and n.get('selected') == 'true']
        state = find(root, 'connection_state')
        if selected != [address.upper()] or state is None or state.get('text') != 'CONNECTED':
            raise RuntimeError('GATT requires the exact target tab to be connected')
        if not any(uuid.lower() in n.get('text', '').lower() for n in root.iter('node')):
            service = next((n for n in root.iter('node') if
                            n.get('resource-id') == PKG + ':id/service_main' and
                            any(SERVICE in child.get('text', '').lower() for child in n.iter())), None)
            if service is None:
                raise RuntimeError('Test service missing')
            self.tap(service)
            root = self.ui('service-expanded')
        if payload is not None:
            self.tap(find(characteristic(root, uuid), 'action_write'))
            root = self.ui('write-dialog')
            if not any(n.get('text') == 'BYTE ARRAY' for n in root.iter('node')):
                raise RuntimeError('Write dialog must use BYTE ARRAY format; no settings were changed')
            root = self.ui('write-payload', payload.hex())
            entry = find(root, 'value')
            if entry is None:
                raise RuntimeError('GATT payload field missing')
            input_text = entry.get('text', '')
            entered = b'' if not payload and input_text.strip() in ('', 'New value') else \
                bytes.fromhex(input_text.replace('-', '').replace(' ', ''))
            if entered != payload:
                raise RuntimeError('GATT write payload UI verification failed')
            send = next((n for n in root.iter('node') if n.get('resource-id') == 'android:id/button1'
                         and n.get('text') == 'SEND'), None)
            self.tap(send)
            time.sleep(1)
            root = self.ui('write-sent')
        self.tap(find(characteristic(root, uuid), 'action_read'))
        time.sleep(0.5)
        root = self.ui('gatt-read')
        data = value_bytes(root, uuid)
        report.update(uuid=uuid, read_hex=data.hex(), success=False)
        if uuid.endswith('02'):
            if len(data) != 4:
                raise RuntimeError('Malformed uptime')
            report['uptime_seconds'] = int.from_bytes(data, 'little')
            report['success'] = True
        elif uuid.endswith('03'):
            if payload is not None:
                report['written_hex'] = payload.hex()
            report['success'] = payload is None or data == payload
        elif uuid.endswith('04'):
            report['phy_status'] = list(data)
            report['success'] = len(data) == 6 and data[0] == 1
            if payload is not None:
                report['success'] = report['success'] and data[1:3] == payload * 2 and data[3:5] == b'\0\0'
        self.screenshot('gatt-result')
        emit('gatt_result', uuid=uuid, read_hex=data.hex(), success=report['success'])
        return 0 if report['success'] else 6


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('command', choices=['inspect', 'scan', 'connect', 'disconnect', 'gatt'])
    p.add_argument('--adb')
    p.add_argument('--sdk', help='Android SDK directory (builds a temporary shell test JAR, no APK)')
    p.add_argument('--serial')
    p.add_argument('--name', default='CH32V208-NIMBLE', help='exact target name')
    p.add_argument('--address', help='also require this BLE address (case insensitive)')
    p.add_argument('--uuid', default=SERVICE[:-2] + '02', help='test GATT characteristic UUID')
    p.add_argument('--write-hex', help='write test echo or PHY, then read and verify')
    p.add_argument('--close', action='store_true', help='close this target tab after disconnect')
    p.add_argument('--duration', type=float, default=20, help='scanner observation seconds (3..30)')
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    if args.close and args.command != 'disconnect':
        p.error('--close requires disconnect')
    try:
        payload = bytes.fromhex(args.write_hex) if args.write_hex is not None else None
    except ValueError:
        p.error('--write-hex requires complete hexadecimal byte pairs')
    args.uuid = args.uuid.lower()
    if args.command == 'gatt' and (not args.address or args.uuid not in
            [SERVICE[:-2] + suffix for suffix in ('02', '03', '04')]):
        p.error('gatt requires --address and a test service characteristic UUID')
    if payload is not None and (args.command != 'gatt' or len(payload) > 64 or
            args.uuid.endswith('02') or args.uuid.endswith('04') and payload not in (b'\x01', b'\x02')):
        p.error('write supports echo (<=64 bytes) or PHY (01/02) only')
    if not 3 <= args.duration <= 30:
        p.error('--duration must be 3..30 seconds (preserves the app scan timeout setting)')
    adb = adb_path(args.adb)
    devices = subprocess.check_output([adb, 'devices'], text=True).splitlines()[1:]
    online = [line.split()[0] for line in devices if len(line.split()) >= 2 and line.split()[1] == 'device']
    serial = args.serial or (online[0] if len(online) == 1 else None)
    if not serial or serial not in online:
        raise RuntimeError('Select one authorized phone with --serial; check adb devices')
    out = (args.output or HERE / '.build' / dt.datetime.now().strftime('phone-%Y%m%d-%H%M%S')).resolve()
    out.mkdir(parents=True, exist_ok=False)
    phone = Phone(adb, serial, out, build_helper(args.sdk))
    report = dict(command=args.command, target=args.name, address=args.address,
                  serial=serial, output=str(out), success=False,
                  tool_source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  helper_source_sha256=hashlib.sha256((HERE / 'ShellUi.java').read_bytes()).hexdigest())
    elf = HERE.parents[1] / 'build/ch32v208_nimble.elf'
    if elf.is_file():
        report['local_build_elf_sha256'] = hashlib.sha256(elf.read_bytes()).hexdigest()
    scanning = False
    hci_baseline = None
    try:
        report['phone'] = phone.inspect()
        initial_hci = hci_summary((out / 'bluetooth.txt').read_text(), args.name, args.address)
        hci_baseline = initial_hci.get('last_timestamp_us')
        phone.run('shell', 'am', 'start', '-n', PKG + '/.MainActivity')
        root = phone.ui('initial')
        if args.command in ('scan', 'connect') and find(root, 'connection_state') is None:
            scanner = next((n for n in root.iter('node') if n.get('content-desc') == 'Scanner'), None)
            if scanner is not None and scanner.get('clickable') == 'true':
                phone.tap(scanner)
                root = phone.ui('scanner')
        report['initial_filters'] = {key: n.get('text') for key in ('filter_header', 'filter_excludes')
                                     if (n := find(root, key)) is not None}
        emit('phone', **report['phone'], filters=report['initial_filters'], output=str(out))
        if args.command == 'inspect':
            phone.screenshot('inspect')
            report['success'] = True
            return 0
        if args.command == 'gatt':
            return phone.gatt(root, args.address, args.uuid, payload, report)
        if args.command in ('connect', 'disconnect') and find(root, 'connection_state') is not None:
            if not args.address:
                raise RuntimeError('Existing device tabs require --address for target verification')
            return phone.connection(root, args.address, report, args.command == 'disconnect', args.close)
        if args.command == 'disconnect':
            raise RuntimeError('Select the target device tab before disconnecting')
        stop = find(root, 'action_scan_stop')
        if stop is not None:
            phone.tap(stop)
            root = phone.ui('stopped-existing-scan')
        # Filters belong to the user. Never open, clear, edit or restore them.
        report['scan_filters'] = {key: n.get('text') for key in ('filter_header', 'filter_excludes')
                                  if (n := find(root, key)) is not None}
        scan_button = find(root, 'action_scan_start')
        phone.tap(scan_button)
        scanning = True
        active_root = phone.ui('scan-active')
        active_stop = find(active_root, 'action_scan_stop')
        if active_stop is None:
            phone.screenshot('unexpected-scanner-state')
            raise RuntimeError('Active scanner STOP control missing; see screenshot/XML')
        started = time.monotonic()
        seen = {}
        # Active duplicate reports can prevent UIAutomator's idle wait from
        # succeeding. Observe the screen, then pause using the same toolbar
        # control; only parse a newly generated hierarchy after pausing.
        time.sleep(args.duration)
        phone.screenshot('active-scan')
        phone.tap(active_stop)
        scanning = False
        root = phone.ui('scan-finished')
        if find(root, 'action_scan_start') is None:
            raise RuntimeError('Scanner did not return to SCAN after pause')
        for fields, _ in rows(root):
            if fields['display_name'] == args.name and (
                not args.address or fields['address'].upper() == args.address.upper()):
                seen[fields['address']] = fields
                emit('target_visible', **fields)
        phone.screenshot('scan-finished')
        report['final_filters'] = {key: n.get('text') for key in ('filter_header', 'filter_excludes')
                                   if (n := find(root, key)) is not None}
        report['filters_preserved'] = report['initial_filters'] == report['final_filters']
        report['hci'] = phone.hci(args.name, args.address, hci_baseline)
        fresh = not report['hci']['available'] or bool(report['hci']['targets'])
        report.update(observed_seconds=time.monotonic() - started, targets=list(seen.values()),
                      success=bool(seen) and fresh,
                      evidence='active-scan screenshot, new paused UI XML and HCI reports after baseline when available')
        if args.command == 'connect':
            matches = [(fields, node) for fields, node in rows(root)
                       if fields['display_name'] == args.name and (
                           not args.address or fields['address'].upper() == args.address.upper())]
            if len(matches) != 1 or not seen:
                raise RuntimeError('Connection requires exactly one visible matching target; use --address')
            phone.tap(find(matches[0][1], 'action_connect'))
            root = phone.ui('connection')
            return phone.connection(root, matches[0][0]['address'], report, requested=True)
        return 0 if report['success'] else 4
    finally:
        if scanning:
            try:
                root = phone.ui('cleanup')
                stop = find(root, 'action_scan_stop')
                if stop is not None:
                    phone.tap(stop)
            except Exception as error:
                report['cleanup_error'] = str(error)
        try:
            phone.logs()
            if 'connection_state' in report:
                (out / 'connection-bluetooth.txt').write_text(phone.run('shell', 'dumpsys', 'bluetooth_manager'))
            if 'hci' not in report:
                report['hci'] = phone.hci(args.name, args.address, hci_baseline)
        except Exception as error:
            report['log_error'] = str(error)
        (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        emit('summary', **report)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.SubprocessError, ET.ParseError, OSError, ValueError) as error:
        emit('error', message=str(error))
        sys.exit(2)
