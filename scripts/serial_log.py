#!/usr/bin/env python3
"""serial_log.py - read the CH32V208 NimBLE target's UART log stream.

The firmware writes logs on USART1 PA9 at 921600 baud, 8N1, no flow
control. On macOS 921600 is not a standard termios speed: pyserial's
darwin backend applies it via the IOSSIOSPEED ioctl and raises
SerialException if that ioctl is rejected - there is no silent fallback
to 115200. The requested speed is printed at startup. A readable wire capture is the
end-to-end check; the pyserial baudrate property is not a driver readback.

Ctrl-C exits cleanly. Killing or closing this reader does not affect the
target: UART DMA continues without host flow control. Ring overflow from
producer bursts is counted independently of whether a reader is attached.

Usage:
  scripts/serial_log.py --list
  scripts/serial_log.py                      # auto-pick if exactly 1 USB serial
  scripts/serial_log.py -p /dev/cu.usbmodemXXXX
  scripts/serial_log.py -p ... --output capture.log
"""

import argparse
import glob
import os
import signal
import sys

try:
    import serial
except ImportError:  # pragma: no cover
    print("pyserial is required: pip3 install pyserial", file=sys.stderr)
    sys.exit(1)

# Candidate USB-UART device patterns. Bluetooth-Incoming-Port and
# Apple's debug-console are deliberately never touched.
PATTERNS = ("/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.usb-*",
            "/dev/cu.wch*", "/dev/cu.slab*", "/dev/cu.UART*")
EXCLUDE_SUBSTR = ("Bluetooth", "debug-console")

BAUD = 921600


def candidates():
    seen = []
    for pat in PATTERNS:
        for dev in sorted(glob.glob(pat)):
            if any(s in dev for s in EXCLUDE_SUBSTR):
                continue
            if dev not in seen:
                seen.append(dev)
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default=os.environ.get("PORT"),
                    help="serial device (default: auto if exactly one USB serial)")
    ap.add_argument("--list", action="store_true", help="list candidate ports")
    ap.add_argument("-b", "--baud", type=int, default=BAUD,
                    help="baud rate (default %(default)s)")
    ap.add_argument("-o", "--output", metavar="FILE",
                    help="also write the raw byte stream to FILE")
    args = ap.parse_args()

    if args.list:
        for dev in candidates():
            print(dev)
        return 0

    port = args.port
    if port is None:
        cands = candidates()
        if len(cands) == 1:
            port = cands[0]
            print(f"[serial-log] single USB serial found: {port}", file=sys.stderr)
        else:
            print("multiple/no candidate ports; pick one with -p/--port:",
                  file=sys.stderr)
            for dev in cands:
                print(f"  {dev}", file=sys.stderr)
            return 2
    elif not os.path.exists(port):
        print(f"port not found: {port}", file=sys.stderr)
        return 2

    out = None
    if args.output:
        out = open(args.output, "wb")

    ser = serial.Serial(port=None)  # configure before open
    ser.port = port
    ser.baudrate = args.baud
    ser.bytesize = serial.EIGHTBITS
    ser.parity = serial.PARITY_NONE
    ser.stopbits = serial.STOPBITS_ONE
    ser.rtscts = False
    ser.dsrdtr = False
    ser.timeout = 0.5
    try:
        ser.open()
    except serial.SerialException as e:
        print(f"open {port} failed: {e}", file=sys.stderr)
        return 1

    # pyserial applies non-standard speeds on darwin via the IOSSIOSPEED
    # ioctl during open(); a rejected ioctl raises SerialException, so a
    # silent fallback to 115200 is impossible. The authoritative end-to-end
    # check is a cleanly decoded banner at this speed.
    print(f"[serial-log] {port} @ {ser.baudrate} 8N1 no-flow "
          f"(pyserial darwin IOSSIOSPEED; failure raises)",
          file=sys.stderr)

    def bye(signum, frame):
        print(f"\n[serial-log] closing ({signal.Signals(signum).name})",
              file=sys.stderr)
        raise SystemExit(0)

    signal.signal(signal.SIGINT, bye)
    signal.signal(signal.SIGTERM, bye)

    try:
        while True:
            try:
                data = ser.read(4096)
            except serial.SerialException as e:
                print(f"[serial-log] read failed: {e}", file=sys.stderr)
                return 1
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                if out:
                    out.write(data)
                    out.flush()
    except SystemExit:
        pass
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        if out:
            out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
