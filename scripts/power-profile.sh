#!/usr/bin/env bash
# Build a quiet advertising image; every reset retains a 10-second reflash window.
# GPIO_ANALOG=ON selects the GPIO configuration used for the documented result.
# Optional overrides: SYSCLK_MHZ, POWER_GUARD_US, RF_GATE, CHIP.
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INTERVAL="${1:-1000}"
GPIO_ANALOG="${GPIO_ANALOG:-OFF}"
CHIP="${CHIP:-CH32V208WBU6}"

case "$INTERVAL" in
    100|500|1000|2000|5000) ;;
    *)
        echo 'Interval: 100, 500, 1000, 2000 or 5000 ms' >&2
        exit 2
        ;;
esac
if [[ "${2:-}" != "" && "${2:-}" != "--flash" ]]; then
    echo 'Usage: power-profile.sh INTERVAL_MS [--flash]' >&2
    exit 2
fi

# Keep the measured analogue GPIO profile separate from the pull-down profile.
BUILD_DIR="$PROJ_DIR/build-power-$INTERVAL"
if [[ "$GPIO_ANALOG" == "ON" ]]; then
    BUILD_DIR="$BUILD_DIR-analog"
fi
IMAGE="$BUILD_DIR/ch32v208_nimble.elf"

cmake -S "$PROJ_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$PROJ_DIR/cmake/riscv32-wch.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCH32_SYSCLK_MHZ="${SYSCLK_MHZ:-96}" \
    -DCH32_LOW_POWER=ON \
    -DCH32_POWER_LSE=OFF \
    -DCH32_POWER_DIAGNOSTICS=OFF \
    -DCH32_POWER_RF_GATE="${RF_GATE:-ON}" \
    -DCH32_POWER_GPIO_ANALOG="$GPIO_ANALOG" \
    -DCH32_POWER_WAKE_GUARD_US="${POWER_GUARD_US:-3000}" \
    -DCH32_ADV_INTERVAL_MS="$INTERVAL" \
    -DCH32_BLE_TEST_IDENTITY=ON \
    -DCH32_BLE_PACKET_LOGS=OFF
cmake --build "$BUILD_DIR" --target ch32v208_nimble -j4

if [[ "${2:-}" == "--flash" ]]; then
    probe-rs download --chip "$CHIP" --connect-under-reset --verify "$IMAGE"
    probe-rs reset --chip "$CHIP"
fi
printf 'Power profile: %s ms advertising, GPIO_ANALOG=%s\nImage: %s\n' \
    "$INTERVAL" "$GPIO_ANALOG" "$IMAGE"
