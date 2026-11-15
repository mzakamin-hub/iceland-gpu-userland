#!/usr/bin/env bash
# Compile our test shaders to AIR objects with Apple's Metal compiler, on the macOS runner.
#
#   compile-air.sh <shader-directory> <output-directory>
#
# One <name>.air per <name>.metal: an object file (LLVM bitcode), not a metallib; nothing is linked or archived. The
# flags are shaders/cases.json's metal_flags. The objects are a run artifact for the Linux job; they are never committed.
# METAL_TOOLCHAIN=download lets Xcode fetch its Metal toolchain component when the image lacks it (Xcode 26 and later
# ship it separately); the default is to stop and say so.
set -euo pipefail
src="${1:?usage: compile-air.sh <shader-directory> <output-directory>}"
out="${2:?usage: compile-air.sh <shader-directory> <output-directory>}"
mkdir -p "$out"

if ! xcrun -sdk macosx -f metal >/dev/null 2>&1; then
    if [ "${METAL_TOOLCHAIN:-fail}" = download ]; then
        echo "== no Metal compiler in ${XCODE_APP:-the selected Xcode}: asking Xcode for its Metal toolchain component"
        xcodebuild -downloadComponent MetalToolchain
    fi
    if ! xcrun -sdk macosx -f metal >/dev/null 2>&1; then
        echo "::error::no Metal compiler (xcrun -sdk macosx -f metal) in ${XCODE_APP:-the selected Xcode}; dispatch with metal_toolchain=download, or name an Xcode that has it"
        exit 1
    fi
fi

flags=()
while IFS= read -r flag; do
    [ -n "$flag" ] && flags+=("$flag")
done < <(python3 -c 'import json,sys; print("\n".join(json.load(open(sys.argv[1])).get("metal_flags", [])))' "$src/cases.json")

for source in "$src"/*.metal; do
    name="$(basename "${source%.metal}")"
    xcrun -sdk macosx metal ${flags[@]+"${flags[@]}"} -Wall -c "$source" -o "$out/$name.air"
    echo "== $name.air $(wc -c < "$out/$name.air" | tr -d ' ') bytes"
done
{
    echo "metal: $(xcrun -sdk macosx metal --version 2>&1 | head -n 1)"
    echo "macosx-sdk: $(xcrun -sdk macosx --show-sdk-version) ($(xcrun -sdk macosx --show-sdk-build-version))"
    echo "flags: ${flags[*]-}"
    (cd "$out" && shasum -a 256 ./*.air)
} | tee "$out/TOOLCHAIN.txt"
