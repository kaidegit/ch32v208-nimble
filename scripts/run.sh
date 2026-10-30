#!/usr/bin/env bash
#
# run.sh - flash and reset; leave the BLE target running independently.
#
# probe-rs auto-discovers the WCH-LinkE probe and the RTT control block in
# the ELF. Extra args are forwarded to probe-rs download.
#
# Usage:
#   scripts/run.sh
#   CHIP=CH32V208CBU6 scripts/run.sh    # override chip name
#   scripts/run.sh --verify             # verify the flashed firmware
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

echo "[run] flashing ${ELF}"
probe-rs download --chip "${CHIP}" "$@" "${ELF}"

# Startup's absolute trampoline handles the reset alias. Starting normally
# also avoids leaving an interactive debugger in control of the BLE clock.
probe-rs reset --chip "${CHIP}"
echo "[run] target running; live RTT is intrusive on this CH32/probe-rs path"
