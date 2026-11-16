#!/usr/bin/env bash
# Build BlueTool into $BUILD_DIR. Called by scripts/build-userland.sh on the macOS runner with TARGET
# (arm64e-apple-macos27.0), SDK_PATH and BUILD_DIR set; signing and packing are scripts/package-userland.py's.
#
# BlueTool.c is not ours: it is UniXNU/QcaBluetooth's userland/BlueTool.c at main effe4ec5, byte for byte
# (https://github.com/UniXNU/QcaBluetooth, BSD-2-Clause-Patent; its licence text is beside it). It is the XPC service
# com.apple.BlueTool that macOS's bluetoothd asks to start the controller: it answers RunBuiltinScript boot / init /
# deepsleep from three kernel sysctls (kern.qbt_power_on, kern.qbt_boot, kern.qbt_firmware) and touches no hardware.
# Apple's own BlueTool has no boot script for a board it does not know and leaves without opening the controller.
# The flags and the one entitlement are the upstream build's (tools/build-bluetool.py).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
want=452fbd09d87ef97d4e0537458650122f8e1d3a26bcffb7d99b10c8806bf0481c
have="$(shasum -a 256 "$here/BlueTool.c" | cut -d' ' -f1)"
[ "$have" = "$want" ] || { echo "::error::BlueTool.c differs from the upstream file ($have)"; exit 1; }
xcrun --sdk macosx clang -target "$TARGET" -isysroot "$SDK_PATH" -O2 -Wall -Wextra -Werror -fblocks \
    -fstack-protector-strong -o "$BUILD_DIR/BlueTool" "$here/BlueTool.c"
echo "== BlueTool $(wc -c < "$BUILD_DIR/BlueTool" | tr -d ' ') bytes"
