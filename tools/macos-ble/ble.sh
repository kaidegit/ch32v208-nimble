#!/bin/sh
set -eu
tool_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir="$tool_dir/.build"
binary="$build_dir/macos-ble"
mkdir -p "$build_dir"
if [ ! -x "$binary" ] || [ "$tool_dir/main.swift" -nt "$binary" ] || [ "$tool_dir/Info.plist" -nt "$binary" ] || [ "$0" -nt "$binary" ]; then
  /usr/bin/swiftc -O -framework CoreBluetooth "$tool_dir/main.swift" -o "$binary" \
    -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker "$tool_dir/Info.plist"
  /usr/bin/codesign --force --sign - --identifier local.wch.macos-ble "$binary" >&2
fi
exec "$binary" "$@"
