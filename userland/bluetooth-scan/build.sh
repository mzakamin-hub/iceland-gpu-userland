#!/usr/bin/env bash
# Build iceland-bt-scan into $BUILD_DIR. Called by scripts/build-userland.sh on the macOS runner with TARGET
# (arm64e-apple-macos27.0), SDK_PATH and BUILD_DIR set; signing and packing are scripts/package-userland.py's.
#
# iceland-bt-scan is ours: it waits for the Bluetooth central manager to be powered on, scans a while with no service
# filter and prints one line per new peripheral (an identifier's first 8 hex characters, never a name or an address),
# so a boot with nobody at the tablet can show what the stack hears. Public Foundation / CoreBluetooth calls only,
# Objective-C with ARC. Its Info.plist rides in the binary (__TEXT,__info_plist) for the NSBluetoothAlwaysUsageDescription
# text; its entitlement (install.json: com.apple.private.tcc.allow kTCCServiceBluetoothAlways) holds only because the
# owner trust bundle on the project machine lists the CDHash, as for every tool here.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
xcrun --sdk macosx clang -target "$TARGET" -isysroot "$SDK_PATH" -O2 -Wall -Wextra -Werror -fobjc-arc \
    -framework Foundation -framework CoreBluetooth \
    -sectcreate __TEXT __info_plist "$here/Info.plist" \
    -o "$BUILD_DIR/iceland-bt-scan" "$here/iceland-bt-scan.m"
echo "== iceland-bt-scan $(wc -c < "$BUILD_DIR/iceland-bt-scan" | tr -d ' ') bytes"
