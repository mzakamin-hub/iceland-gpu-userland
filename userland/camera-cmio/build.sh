#!/usr/bin/env bash
# Build the camera's userland into $BUILD_DIR: IcelandCameraHost.app, the little app that asks the system to activate
# the CoreMediaIO camera extension it carries (IcelandCamera, at Contents/Library/SystemExtensions), and
# iceland-camera-probe, the boot-log check. Called by scripts/build-userland.sh on the macOS runner with TARGET
# (arm64e-apple-macos27.0), SDK_PATH and BUILD_DIR set. Every .m and .c file of this folder is compiled into the
# extension, so new sources need no change here (host/ and probe/ are built on their own lines below); add a framework
# to the link line when one is needed. Signing and packing are scripts/package-userland.py's, from install.json: the
# extension inside the app is signed as a bundle of its own first ("nested"), with extension.entitlements, then the app
# with host/host.entitlements.
set -euo pipefail
shopt -s nullglob
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
compile=(xcrun --sdk macosx clang -target "$TARGET" -isysroot "$SDK_PATH" -O2 -Wall -Werror)

# The system looks for an extension at Contents/Library/SystemExtensions/<bundle identifier>.systemextension.
app="$BUILD_DIR/IcelandCameraHost.app"
extension="$app/Contents/Library/SystemExtensions/org.iceland.camera.extension.systemextension"
mkdir -p "$app/Contents/MacOS" "$extension/Contents/MacOS"

# The extension: CoreMediaIO and CoreMedia for the provider, CoreVideo for the pixel buffers, IOKit for the
# IcelandCamera user client, CoreFoundation and IOSurface for what those hand over.
cp "$here/Info.plist" "$extension/Contents/Info.plist"
"${compile[@]}" -fobjc-arc \
    -framework Foundation -framework CoreFoundation -framework CoreMediaIO -framework CoreMedia -framework CoreVideo \
    -framework IOKit -framework IOSurface \
    -o "$extension/Contents/MacOS/IcelandCamera" "$here"/*.m "$here"/*.c

# The host app; the kernel service's client code is shared for its log lines.
cp "$here/host/Info.plist" "$app/Contents/Info.plist"
"${compile[@]}" -fobjc-arc \
    -framework Foundation -framework CoreFoundation -framework SystemExtensions -framework IOKit \
    -o "$app/Contents/MacOS/IcelandCameraHost" "$here/host"/*.m "$here/iceland-camera-kernel.c"

# The probe: a plain C tool.
"${compile[@]}" -std=c11 \
    -framework CoreFoundation -framework CoreMediaIO -framework CoreMedia -framework CoreVideo -framework IOKit \
    -o "$BUILD_DIR/iceland-camera-probe" "$here/probe/iceland-camera-probe.c" "$here/iceland-camera-kernel.c"
