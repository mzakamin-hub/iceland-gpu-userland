#!/usr/bin/env bash
# Build IcelandCamera.systemextension, the CoreMediaIO camera extension, into $BUILD_DIR. Called by
# scripts/build-userland.sh on the macOS runner with TARGET (arm64e-apple-macos27.0), SDK_PATH and BUILD_DIR set. Every
# .m and .c file of this folder is compiled, so new sources need no change here; add a framework to the link line when
# one is needed. Signing and packing are scripts/package-userland.py's, from install.json (an "entitlements" key there
# names a plist of this folder if the extension needs one).
set -euo pipefail
shopt -s nullglob
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bundle="$BUILD_DIR/IcelandCamera.systemextension"
mkdir -p "$bundle/Contents/MacOS"
cp "$here/Info.plist" "$bundle/Contents/Info.plist"
xcrun --sdk macosx clang -target "$TARGET" -isysroot "$SDK_PATH" -O2 -Wall -Werror -fobjc-arc \
    -framework Foundation -framework CoreMediaIO -framework CoreMedia -framework CoreVideo \
    -o "$bundle/Contents/MacOS/IcelandCamera" "$here"/*.m "$here"/*.c
