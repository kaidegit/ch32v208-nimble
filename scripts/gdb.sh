#!/usr/bin/env bash
#
# gdb.sh - expose a GDB remote server (tcp:3333) via probe-rs and print how
# to connect riscv32-wch-elf-gdb to it.  Runs in the foreground; Ctrl-C quits.
#
# Usage:
#   scripts/gdb.sh            # after flashing with scripts/run.sh
#   CHIP=CH32V208RBT6 scripts/gdb.sh
set -euo pipefail

CHIP="${CHIP:-CH32V208WBU6}"
GDB="${GDB:-riscv32-wch-elf-gdb}"

command -v probe-rs >/dev/null || {
    echo "probe-rs not found in PATH (https://probe.rs/)" >&2; exit 1;
}

cat <<EOF
[gdb] starting probe-rs GDB server on tcp:3333 (chip: ${CHIP})

Connect from another terminal:

    ${GDB} build/ch32v208_nimble.elf \\
        -ex 'target remote localhost:3333' \\
        -ex 'set \$sp=0x20010000' \\
        -ex 'set \$pc=0x08000000' \\
        -ex continue

Add source dir lookup if needed:
    directory \$(pwd)
EOF

exec probe-rs gdb --chip "${CHIP}" --gdb-connection-string localhost:3333 "$@"
