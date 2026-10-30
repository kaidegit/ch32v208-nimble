#!/usr/bin/env bash
# Compatibility entry point: runtime logs now use USART1, never probe-rs RTT.
set -euo pipefail
PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "${PROJ_DIR}/scripts/log.sh" "$@"
