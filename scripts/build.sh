#!/usr/bin/env bash
#
# build.sh - configure + build the CH32V208 NimBLE firmware with CMake.
#
# Usage:
#   scripts/build.sh              # Release build into build/
#   scripts/build.sh --clean      # wipe build/ first
#   BUILD_TYPE=Debug scripts/build.sh
#   SYSCLK_MHZ=144 scripts/build.sh  # 72 / 96 / 144 (default cache: 96)
#
# Toolchain: WCH riscv32-wch-elf-gcc; override its location with
#   WCH_TOOLCHAIN_DIR=/path/to/riscv32-wch-elf-gcc-15 scripts/build.sh
# (default: ~/DevTools/riscv32-wch-elf-gcc-15, see cmake/riscv32-wch.toolchain.cmake)
set -euo pipefail

PROJ_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJ_DIR}/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"
CLOCK_ARGS=()
if [[ -n "${SYSCLK_MHZ:-}" ]]; then
    CLOCK_ARGS=("-DCH32_SYSCLK_MHZ=${SYSCLK_MHZ}")
fi
TOOLCHAIN_FILE="${PROJ_DIR}/cmake/riscv32-wch.toolchain.cmake"

if [[ "${1:-}" == "--clean" ]]; then
    echo "[build] removing ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

# Number of parallel jobs: Linux (nproc) or macOS (sysctl -n hw.ncpu).
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

[[ -f "${TOOLCHAIN_FILE}" ]] || { echo "toolchain file missing: ${TOOLCHAIN_FILE}" >&2; exit 1; }
command -v cmake >/dev/null || { echo "cmake not found in PATH" >&2; exit 1; }

echo "[build] configure: ${BUILD_TYPE}, toolchain=${TOOLCHAIN_FILE}"
cmake -S "${PROJ_DIR}" -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
      -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" ${CLOCK_ARGS[@]+"${CLOCK_ARGS[@]}"}

echo "[build] compile with ${JOBS} jobs"
cmake --build "${BUILD_DIR}" -j"${JOBS}"

echo "[build] done. Artifacts:"
ls -l "${BUILD_DIR}"/ch32v208_nimble.{elf,hex,bin} 2>/dev/null || true
