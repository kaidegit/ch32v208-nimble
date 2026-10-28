#!/usr/bin/env bash
#
# attach.sh - attach probe-rs RTT console to a RUNNING target (no flashing).
#
# The board must already run the firmware (e.g. flashed earlier with
# scripts/run.sh).  Use this to watch logs without disturbing execution.
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

[[ -f "${ELF}" ]] || {
    echo "firmware not found: ${ELF} (needed for the RTT control block symbol)" >&2; exit 1;
}

exec probe-rs attach --chip "${CHIP}" "$@" "${ELF}"
