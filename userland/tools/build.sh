#!/usr/bin/env bash
# Build the small helper programs into $BUILD_DIR. Called by scripts/build-userland.sh on the macOS runner with TARGET
# (arm64e-apple-macos27.0), SDK_PATH and BUILD_DIR set. One `tool` line per program here and one product in
# install.json; signing and packing are scripts/package-userland.py's.
#
#   tool <output name> <frameworks, space separated> <extra flags, may be empty> <source> [<source> ...]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

tool() {
    local name="$1" frameworks="$2" extra="$3"
    shift 3
    local flags=() framework
    for framework in $frameworks; do flags+=(-framework "$framework"); done
    # shellcheck disable=SC2086
    xcrun --sdk macosx clang -target "$TARGET" -isysroot "$SDK_PATH" -Wall -Werror $extra \
        ${flags[@]+"${flags[@]}"} -o "$BUILD_DIR/$name" "$@"
    echo "== $name $(wc -c < "$BUILD_DIR/$name" | tr -d ' ') bytes"
}

# UniXNU's absent-service stand-in: the copied, hash-pinned source and the flags of the `absentd` stage of the GPU
# workflow (which stays the build the S8 kits' hash names; this one is for a rebuild with the other tools).
python3 -c 'import hashlib, json, sys; p = json.load(open(sys.argv[1] + "/pins/inputs.json"))["sources"]["absentd"]; a = hashlib.sha256(open(sys.argv[1] + "/" + p["path"], "rb").read()).hexdigest(); assert a == p["sha256"], "the copied absentd source differs from its pin"' "$repo"
tool unixnu-absentd "CoreFoundation" "-Os -Wextra -fblocks" "$repo/absent-service/absentd.c"

# Ours: lists the audio devices coreaudiod publishes (a boot-log check for the HAL plug-in).
tool iceland-audio-probe "CoreAudio CoreFoundation" "-std=c11 -O2" "$here/iceland-audio-probe.c"
