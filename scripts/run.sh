#!/usr/bin/env bash
#
# run.sh - flash + run the firmware under probe-rs and stream RTT logs.
#
# probe-rs (>= 0.27) auto-discovers the WCH-LinkE probe and the RTT control
# block in the ELF, so SEGGER RTT output is forwarded to this terminal.
#
# Usage:
#   scripts/run.sh
#   CHIP=CH32V208CBU6 scripts/run.sh    # override chip name
#   scripts/run.sh --noreset            # extra args are forwarded to probe-rs
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ELF="${PROJ_DIR}/build/ch32v208_nimble.elf"
CHIP="${CHIP:-CH32V208WBU6}"

command -v probe-rs >/dev/null || {
    echo "probe-rs not found in PATH (https://probe.rs/)" >&2; exit 1;
}

echo "[run] probes detected:"
probe-rs list || true
echo

[[ -f "${ELF}" ]] || {
    echo "firmware not found: ${ELF} (run scripts/build.sh first)" >&2; exit 1;
}

# probe-rs run = flash + reset + attach RTT; Ctrl-C detaches.
exec probe-rs run --chip "${CHIP}" "$@" "${ELF}"
