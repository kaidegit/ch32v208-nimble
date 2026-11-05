#!/usr/bin/env bash
# Build an isolated power demo. Every image has a mandatory 10 s boot window.
set -euo pipefail
PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-stop-event}"
case "$MODE" in
    run) MODE_ID=0 ;;
    sleep) MODE_ID=1 ;;
    stop-irq) MODE_ID=2 ;;
    stop-masked-wfi) MODE_ID=3 ;;
    stop-event) MODE_ID=4 ;;
    stop-pending) MODE_ID=5 ;;
    *) echo 'Modes: run sleep stop-irq stop-masked-wfi stop-event stop-pending' >&2; exit 2 ;;
esac
if [[ "${2:-}" != "" && "${2:-}" != "--flash" ]]; then
    echo 'Usage: low-power-demo.sh MODE [--flash]' >&2; exit 2
fi
BUILD_SUFFIX=""
if [[ "${LP_PARK_IO:-OFF}" == "ON" ]]; then BUILD_SUFFIX="-park-io"; fi
BUILD_DIR="${PROJ_DIR}/build-lp-${MODE}${BUILD_SUFFIX}"
cmake -S "$PROJ_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$PROJ_DIR/cmake/riscv32-wch.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCH32_SYSCLK_MHZ="${SYSCLK_MHZ:-96}" \
    -DCH32_LP_MODE="$MODE_ID" -DCH32_LP_PERIOD="${LP_PERIOD:-5}" \
    -DCH32_LP_LSE="${LP_LSE:-OFF}" -DCH32_LP_PARK_IO="${LP_PARK_IO:-OFF}"
cmake --build "$BUILD_DIR" --target low_power_demo -j8
if [[ "${2:-}" == "--flash" ]]; then
    probe-rs download --chip "${CHIP:-CH32V208WBU6}" --verify "$BUILD_DIR/low_power_demo.elf"
    probe-rs reset --chip "${CHIP:-CH32V208WBU6}"
fi
printf 'Image: %s/low_power_demo.elf\nUART: PA9 TX, 115200 8N1\n' "$BUILD_DIR"
