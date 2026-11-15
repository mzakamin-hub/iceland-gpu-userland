#!/usr/bin/env bash
# Build IcelandAudio.driver, the CoreAudio AudioServerPlugIn, into $BUILD_DIR. Called by scripts/build-userland.sh on
# the macOS runner with TARGET (arm64e-apple-macos27.0), SDK_PATH and BUILD_DIR set. Every .c file of this folder is
# compiled, so new sources need no change here; add a framework to the link line when one is needed. Signing and
# packing are scripts/package-userland.py's, from install.json.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bundle="$BUILD_DIR/IcelandAudio.driver"
mkdir -p "$bundle/Contents/MacOS"
cp "$here/Info.plist" "$bundle/Contents/Info.plist"
xcrun --sdk macosx clang -target "$TARGET" -isysroot "$SDK_PATH" -std=c11 -O2 -Wall -Werror -fvisibility=hidden \
    -bundle -framework CoreAudio -framework CoreFoundation \
    -o "$bundle/Contents/MacOS/IcelandAudio" "$here"/*.c
# The factory must be the one exported symbol Info.plist names.
xcrun nm -gU "$bundle/Contents/MacOS/IcelandAudio" | grep -q ' _IcelandAudio_Create$'
