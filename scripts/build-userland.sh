#!/usr/bin/env bash
# Build, sign ad hoc and pack one userland component on the macOS runner.
#
#   build-userland.sh <component> <artifact-directory>        component: a folder name under userland/
#
# The component's build.sh compiles for arm64e-apple-macos27.0 against the macOS SDK select-xcode.sh chose and leaves its
# products in $BUILD_DIR; scripts/package-userland.py then signs each Mach-O ad hoc (4 KiB pages, no key), records its
# CDHash and writes <artifact-directory>/<component>/ with userland-manifest.json. Nothing is installed and nothing
# is signed with any identity here.
set -euo pipefail
component="${1:?usage: build-userland.sh <component> <artifact-directory>}"
artifacts="${2:?usage: build-userland.sh <component> <artifact-directory>}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$component" in
    *[!a-z0-9-]*|'') echo "::error::component: lower-case letters, digits and dashes"; exit 1 ;;
esac
dir="$here/userland/$component"
[ -f "$dir/build.sh" ] && [ -f "$dir/install.json" ] || { echo "::error::userland/$component needs build.sh and install.json"; exit 1; }
: "${SDK_PATH:?select-xcode.sh must run first}"
export TARGET=arm64e-apple-macos27.0
export BUILD_DIR="${RUNNER_TEMP:-/tmp}/userland-build/$component"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" "$artifacts/$component"
echo "== userland/$component for $TARGET with ${XCODE_APP:-Xcode}, SDK ${SDK_VERSION:-unknown}"
bash "$dir/build.sh"
python3 "$here/scripts/package-userland.py" "$dir" "$BUILD_DIR" "$artifacts/$component"
# Every Mach-O must be what the tablet runs: arm64e, built for macOS 27.
while IFS= read -r -d '' file; do
    if [ "$(head -c 4 "$file" | od -An -tx1 | tr -d ' \n')" = cffaedfe ]; then
        xcrun vtool -show-build "$file" | grep -E 'platform|minos' | tr -s ' ' | tr '\n' ';'
        echo " $(lipo -archs "$file") ${file#"$BUILD_DIR"/}"
        [ "$(lipo -archs "$file")" = arm64e ] || { echo "::error::$file is not arm64e"; exit 1; }
    fi
done < <(find "$BUILD_DIR" -type f -print0)
