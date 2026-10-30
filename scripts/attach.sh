#!/usr/bin/env bash
#
# attach.sh - attach probe-rs RTT console to a RUNNING target (no flashing).
#
# The board must already run the firmware (e.g. flashed earlier with
# scripts/run.sh). On this CH32/probe-rs path RAM reads halt the CPU:
# use for stopped-target diagnostics, not BLE timing validation.
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ELF="${PROJ_DIR}/build/ch32v208_nimble.elf"
CHIP="${CHIP:-CH32V208WBU6}"

command -v probe-rs >/dev/null || {
    echo "probe-rs not found in PATH (https://probe.rs/)" >&2; exit 1;
}

echo "[attach] probes detected:"
probe-rs list || true
echo
echo "[attach] RTT ProgramBuffer reads pause CPU and interfere with BLE"

[[ -f "${ELF}" ]] || {
    echo "firmware not found: ${ELF} (needed for the RTT control block symbol)" >&2; exit 1;
}

# probe-rs otherwise overrides the firmware's nonblocking setting with
# block-if-full. A paused/closed viewer must not stall BLE inside a log call.
exec probe-rs attach --chip "${CHIP}" --rtt-channel-mode no-block-skip \
    --no-catch-reset --no-catch-hardfault "$@" "${ELF}"
