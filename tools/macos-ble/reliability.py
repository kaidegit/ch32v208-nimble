#!/usr/bin/env python3
"""Run BLE reset/reconnect/continuous-link tests; preserve the first failure."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
CLIENT = ROOT / 'tools/macos-ble/ble.sh'


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def emit(event, **fields):
    print(json.dumps(dict(event=event, time=utc(), **fields)), flush=True)


def capture(out, chip, gdb):
    """Called only after a failed test; halts the board and leaves RAM intact."""
    server_log = out / 'probe-gdb.log'
    snapshot = out / 'failure.gdb.txt'
    with server_log.open('w') as sf:
        server = subprocess.Popen(['probe-rs', 'gdb', '--chip', chip,
                                   '--gdb-connection-string', 'localhost:3333'],
                                  cwd=ROOT, stdout=sf, stderr=subprocess.STDOUT)
        try:
            time.sleep(1)
            if server.poll() is not None:
                raise RuntimeError('probe-rs GDB server exited; see probe-gdb.log')
            commands = ['target remote localhost:3333',
                        'set mem inaccessible-by-default off', 'p xTickCount',
                        'p g_config_assert_line', 'p/x g_exc_trap_words',
                        'p g_ble_phy_stats', 'p g_wch_rf_analog_stage',
                        'p/x g_wch_rf_analog_status', 'p g_wch_rf_tune_ready_mask',
                        'p g_wch_rf_tune_expired_mask', 'p g_wch_rf_tune_started',
                        'p g_wch_rf_tune_finished', 'p g_wch_rf_tune_window',
                        'p g_wch_rf_tune_iterations', 'p g_wch_timer',
                        'p g_ble_phy_data', 'p/x g_wch_phy_control_trace',
                        'x/32wx 0x40024200',
                        f'dump binary memory {json.dumps(str(out / "failure.ram.bin"), ensure_ascii=False)} 0x20000000 0x20010000',
                        'detach']
            cmd = [gdb, '-q', '-batch', str(ROOT / 'build/ch32v208_nimble.elf')]
            for command in commands:
                cmd += ['-ex', command]
            with snapshot.open('w') as f:
                result = subprocess.run(cmd, cwd=ROOT, stdout=f,
                                        stderr=subprocess.STDOUT, timeout=40)
            return dict(log=str(snapshot), exit_code=result.returncode,
                        core_left_halted=True)
        finally:
            server.terminate()
            try:
                server.wait(5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--phase', choices=['all', 'resets', 'reconnect', 'soak'], default='all')
    p.add_argument('--duration', type=float, default=1800, help='seconds per PHY in continuous-link tests')
    p.add_argument('--resets', type=int, default=30)
    p.add_argument('--reconnects', type=int, default=100)
    p.add_argument('--interval', type=float, default=0.2)
    p.add_argument('--phys', nargs='+', choices=['1m', '2m'], default=['1m', '2m'])
    p.add_argument('--chip', default='CH32V208WBU6')
    p.add_argument('--id', help='exact CoreBluetooth UUID; default exact board name')
    p.add_argument('--output', type=Path)
    p.add_argument('--gdb', default='/tmp/gdb-wch-fixed')
    args = p.parse_args()
    if not (0 < args.duration <= 86300 and 0 <= args.interval <= 10 and
            0 <= args.resets <= 10000 and 0 <= args.reconnects <= 10000):
        p.error('invalid duration, interval or count')
    if (args.phase == 'resets' and args.resets == 0 or
            args.phase == 'reconnect' and args.reconnects == 0):
        p.error('selected phase requires at least one test')
    out = (args.output or ROOT / 'tools/macos-ble/.build' /
           dt.datetime.now(dt.timezone.utc).strftime('reliability-%Y%m%dT%H%M%SZ')).resolve()
    out.mkdir(parents=True, exist_ok=False)
    elf = ROOT / 'build/ch32v208_nimble.elf'
    report = dict(started=utc(), finished=None, passed=False, parameters=vars(args).copy(),
                  revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  working_changes=subprocess.check_output(['git', 'status', '--short'], cwd=ROOT, text=True),
                  elf_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(), tests=[])
    report['parameters']['output'] = str(out)
    report['runner_source_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    report['process_id'] = os.getpid()
    report['client_source_sha256'] = hashlib.sha256((ROOT / 'tools/macos-ble/main.swift').read_bytes()).hexdigest()
    report['firmware_test_identity'] = next((line for line in (ROOT / 'build/CMakeCache.txt').read_text().splitlines()
                                             if line.startswith('CH32_BLE_TEST_IDENTITY:')), None)
    def save():
        temp = out / 'report.tmp'
        temp.write_text(json.dumps(report, indent=2, default=str) + '\n')
        temp.replace(out / 'report.json')
    save()
    target = ['--id', args.id] if args.id else ['--name', 'CH32V208-NIMBLE']
    failed = False
    awake = subprocess.Popen(['caffeinate', '-i', '-w', str(os.getpid())]) if shutil.which('caffeinate') else None
    try:
        for phase in (['resets', 'reconnect', 'soak'] if args.phase == 'all' else [args.phase]):
            count = {'resets': args.resets, 'reconnect': args.reconnects, 'soak': len(args.phys)}[phase]
            for index in range(count):
                phy = args.phys[index % len(args.phys)]
                label = f'{phase}-{index:04d}-{phy}'
                if phase == 'resets':
                    reset = subprocess.run(['probe-rs', 'reset', '--chip', args.chip],
                                           cwd=ROOT, capture_output=True, text=True, timeout=15)
                    if reset.returncode:
                        report['tests'].append(dict(label=label, passed=False,
                            reset_exit=reset.returncode, reset_output=reset.stdout + reset.stderr))
                        failed = True
                        break
                timeout = args.duration + 30 if phase == 'soak' else 15
                cmd = [str(CLIENT), 'probe', *target, '--phy', phy, '--timeout', str(timeout)]
                if phase == 'soak':
                    cmd += ['--duration', str(args.duration), '--interval', str(args.interval)]
                else:
                    length = [0, 1, 3, 20, 63, 64][index % 6]
                    payload = bytes((index * 131 + j * 17) & 255 for j in range(length))
                    cmd += ['--write-hex', payload.hex()]
                logfile = out / f'{label}.jsonl'
                emit('test_started', label=label, log=str(logfile))
                started = time.monotonic()
                with logfile.open('w') as log:
                    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
                    last_notice = started
                    try:
                        while proc.poll() is None:
                            now = time.monotonic()
                            if now - started > timeout + 10:
                                proc.send_signal(signal.SIGINT)
                                try:
                                    proc.wait(5)
                                except subprocess.TimeoutExpired:
                                    proc.kill()
                                break
                            if now - last_notice >= 30:
                                latest = []
                                for line in logfile.read_text().splitlines()[-15:]:
                                    try:
                                        event = json.loads(line)
                                    except ValueError:
                                        continue
                                    if event.get('event') == 'soak_round':
                                        latest.append(event)
                                emit('test_progress', label=label,
                                     elapsed_seconds=round(now-started, 1),
                                     last_round=latest[-1] if latest else None)
                                last_notice = now
                            time.sleep(0.5)
                    except BaseException:
                        proc.send_signal(signal.SIGINT)
                        try:
                            proc.wait(5)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                            proc.wait()
                        raise
                    code = proc.wait()
                events = []
                for line in logfile.read_text().splitlines():
                    try:
                        events.append(json.loads(line))
                    except ValueError:
                        pass
                summaries = [e for e in events if e.get('event') == 'summary']
                summary = summaries[-1] if summaries else {}
                expected = 'soak_complete' if phase == 'soak' else 'probe_complete'
                passed = code == 0 and summary.get('exit_code') == 0 and summary.get('reason') == expected
                if phase == 'soak':
                    passed = passed and summary.get('soak_elapsed_seconds', 0) >= args.duration
                record = dict(label=label, passed=passed, exit_code=code,
                              elapsed_seconds=time.monotonic()-started, log=str(logfile), summary=summary)
                report['tests'].append(record)
                save()
                emit('test_finished', **record)
                if not passed:
                    failed = True
                    break
            if failed:
                break
        if failed and shutil.which('probe-rs') and Path(args.gdb).exists():
            try:
                report['failure_capture'] = capture(out, args.chip, args.gdb)
                emit('failure_captured', **report['failure_capture'])
            except Exception as exc:
                report['capture_error'] = str(exc)
        report['passed'] = not failed
    except (KeyboardInterrupt, Exception) as exc:
        report['error'] = str(exc) or type(exc).__name__
        failed = True
    finally:
        if awake is not None:
            awake.terminate()
            awake.wait()
        report['finished'] = utc()
        save()
    emit('run_finished', passed=report['passed'], tests=len(report['tests']), report=str(out/'report.json'))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
