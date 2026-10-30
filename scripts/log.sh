#!/usr/bin/env bash
#
# log.sh - daily UART log reader for the CH32V208 NimBLE target.
#
# The firmware logs on USART1 PA9 at 921600 baud 8N1 (DMA async TX, no
# flow control). This replaces probe-rs RTT as the runtime log path and
# is NOT intrusive: closing it (Ctrl-C) does not disturb the target; the
# UART keeps sending without host flow control. Producer bursts can fill
# the on-board ring; those drops are counted.
#
# Usage:
#   scripts/log.sh --list                 # list candidate USB-UART ports
#   scripts/log.sh                        # auto-pick if exactly one
#   scripts/log.sh -p /dev/cu.usbmodemXXX # explicit port
#   scripts/log.sh -o capture.log         # tee raw bytes to a file
#
# Physical wiring note: the reader expects a USB-UART adapter on PA9.
# A missing adapter is a wiring gap, not a pass/fail of the firmware.
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "${PROJ_DIR}/scripts/serial_log.py" "$@"
