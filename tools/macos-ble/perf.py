#!/usr/bin/env python3
"""Run verified GATT and LE CoC echo performance cases; preserve every result."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
from datetime import datetime, timezone

TOOL = Path(__file__).resolve().parent
ROOT = TOOL.parent.parent


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(*cmd):
    return subprocess.check_output(cmd, cwd=ROOT, text=True).strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--duration', type=float, default=30)
    ap.add_argument('--repeats', type=int, default=3)
    ap.add_argument('--transports', nargs='+', choices=['gatt', 'l2cap'], default=['gatt', 'l2cap'])
    ap.add_argument('--phys', nargs='+', choices=['1m', '2m'], default=['1m', '2m'])
    ap.add_argument('--gatt-size', type=int, default=244)
    ap.add_argument('--l2cap-size', type=int, default=512)
    ap.add_argument('--directions', nargs='+', choices=['up', 'down'], default=['up', 'down'])
    ap.add_argument('--id', help='Exact CoreBluetooth UUID')
    args = ap.parse_args()
    if not (0 < args.duration <= 3600 and 1 <= args.repeats <= 100 and
            4 <= args.gatt_size <= 244 and 1 <= args.l2cap_size <= 512):
        ap.error('invalid duration, repeats or payload size')
    elf = ROOT / 'build/ch32v208_nimble.elf'
    if not elf.is_file():
        ap.error('build/ch32v208_nimble.elf missing; build and flash first')
    # Build/self-test before measuring; never rebuild during a case.
    subprocess.run([str(TOOL / 'ble.sh'), '--self-test'], cwd=ROOT, check=True)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    out = TOOL / '.build' / f'perf-{stamp}'
    out.mkdir(parents=True)
    shutil.copy2(elf, out / 'firmware.elf')
    for name in ['main.swift', 'perf.py', 'ble.sh']:
        shutil.copy2(TOOL / name, out / name)
    (out / 'changes.patch').write_text(capture('git', 'diff', 'HEAD'))
    report = {
        'started_utc': stamp, 'options': vars(args), 'status': 'running',
        'revision': capture('git', 'rev-parse', 'HEAD'),
        'working_changes': capture('git', 'status', '--short'),
        'nimble_revision': capture('git', '-C', '../nimble', 'rev-parse', 'HEAD'),
        'nimble_working_changes': capture('git', '-C', '../nimble', 'status', '--short'),
        'firmware_sha256': sha256(out / 'firmware.elf'),
        'client_sha256': sha256(TOOL / 'main.swift'),
        'runner_sha256': sha256(Path(__file__)),
        'macos': capture('sw_vers'), 'machine': platform.machine(),
        'model': capture('sysctl', '-n', 'hw.model'),
        'cmake_settings': [line for line in (ROOT / 'build/CMakeCache.txt').read_text().splitlines()
                           if line.startswith(('CH32_SYSCLK_MHZ:', 'CH32_BLE_TEST_IDENTITY:', 'CH32_BLE_PACKET_LOGS:'))],
        'metric': 'one-way receiver-verified payload bytes / receiver measurement seconds; payload_kbps = bytes * 8 / seconds / 1000',
        'cases': [],
    }

    def save():
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    print(f'Results: {out}', flush=True)
    save()
    caffeinate = subprocess.Popen(['caffeinate', '-i', '-w', str(os.getpid())])
    try:
        # Alternate transports/rates each repetition rather than batching one rate.
        for repeat in range(1, args.repeats + 1):
            for transport in args.transports:
                for phy in args.phys:
                    for direction in args.directions:
                        name = f'{repeat:02d}-{transport}-{phy}-{direction}'
                        size = args.gatt_size if transport == 'gatt' else args.l2cap_size
                        command = [str(TOOL / 'ble.sh'), 'probe' if transport == 'gatt' else 'l2cap',
                                   '--perf', '--phy', phy, '--direction', direction, '--duration', str(args.duration),
                                   '--timeout', str(args.duration + 30), '--size', str(size)]
                        if args.id:
                            command += ['--id', args.id]
                        with (out / f'{name}.jsonl').open('w') as log, (out / f'{name}.stderr').open('w') as err:
                            result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=err,
                                                    timeout=args.duration + 40)
                        events = [json.loads(line) for line in (out / f'{name}.jsonl').read_text().splitlines()]
                        summaries = [e for e in events if e.get('event') == 'summary']
                        summary = summaries[-1] if summaries else {}
                        phy_events = [e for e in events if e.get('event') == 'phy_status' and e.get('pending') is False
                                      and e.get('tx_phy') == (1 if phy == '1m' else 2)
                                      and e.get('rx_phy') == (1 if phy == '1m' else 2) and e.get('status') == 0]
                        passed = (result.returncode == 0 and summary.get('exit_code') == 0 and
                                  summary.get('transfer_seconds', 0) >= args.duration - 0.25 and
                                  summary.get('verified_bytes', 0) > 0 and summary.get('failures') == 0 and
                                  len(phy_events) >= 2)
                        case = {'name': name, 'transport': transport, 'phy': phy, 'repeat': repeat,
                                'direction': direction, 'size': size, 'command': command, 'returncode': result.returncode,
                                'passed': passed, 'summary': summary}
                        report['cases'].append(case)
                        save()
                        print(f'{name}: {"PASS" if passed else "FAIL"}, '
                              f'{summary.get("payload_bytes_per_second", 0):.1f} payload B/s', flush=True)
                        if not passed:
                            report['status'] = 'failed'
                            return 1
        report['status'] = 'passed'
        return 0
    except (KeyboardInterrupt, subprocess.TimeoutExpired) as error:
        report['status'] = 'interrupted' if isinstance(error, KeyboardInterrupt) else 'runner_timeout'
        return 130 if isinstance(error, KeyboardInterrupt) else 1
    except Exception as error:
        report['status'] = 'runner_error'
        report['error'] = str(error)
        raise
    finally:
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
        caffeinate.terminate()
        caffeinate.wait()


if __name__ == '__main__':
    sys.exit(main())
