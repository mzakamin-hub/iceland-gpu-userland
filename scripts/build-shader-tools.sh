#!/usr/bin/env bash
# Build the two compilers of the shader check on a Linux runner, from the sources prepare-inputs.sh fetched at their
# pins (unixnu, mesa, metal2vulkan, venv) and verify-pins.py checked:
#
#   build-shader-tools.sh <products-directory>
#
#   <products>/metal2vulkan         the translator's command-line tool: the pinned metal2vulkan plus the patches the
#                                   Metal plugin's bridge applies (the list in UniXNU's build-adreno-metal-bridge.py, read
#                                   from that file, in its order), so it translates as the plugin on the tablet does
#   <products>/mesa/                Turnip for Linux/msm and Mesa's no-op drm-shim, from the same patched Mesa tree the
#                                   macOS job builds Turnip from (the pin, UniXNU's record, ours), plus one patch of ours
#                                   for this job only: the shim's Adreno 840 entry (pins/inputs.json shader_check)
#   <products>/versions.txt         what was built
#
# Nothing here is signed, shipped or used on the tablet: these are checking tools.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${GITHUB_WORKSPACE:?GITHUB_WORKSPACE must be set}"
PRODUCTS="${1:?usage: build-shader-tools.sh <products-directory>}"
UNIXNU="$WORK/unixnu"
MESA="$UNIXNU/.cache/mesa-work"
M2V="$UNIXNU/.cache/gpu-reference/metal2vulkan"
VENV="$UNIXNU/.cache/mesa-venv/bin"
mkdir -p "$PRODUCTS"

pin() {
    python3 -c 'import json,sys; v=json.load(open(sys.argv[1]))
for k in sys.argv[2].split("."): v=v[k]
print(v)' "$HERE/pins/inputs.json" "$1"
}

# --- the translator, as the bridge builds it
python3 - "$UNIXNU/scripts/build-adreno-metal-bridge.py" > "$PRODUCTS/bridge-patches.txt" <<'LIST'
import re, sys
text = open(sys.argv[1]).read()
block = text[text.index('PATCHES = ['):]
block = block[:block.index(']]') + 2]
names = re.findall(r"'(\d{4}-[A-Za-z0-9_.-]+\.patch)'", block)
if not names:
    sys.exit('no bridge patch list found in build-adreno-metal-bridge.py')
print('\n'.join(names))
LIST
while IFS= read -r name; do
    patch="$UNIXNU/drivers/AdrenoMetal/bridge/patches/$name"
    [ -f "$patch" ] || { echo "::error::the bridge lists $name but AdrenoMetal has no such patch"; exit 1; }
    if git -C "$M2V" apply --reverse --check "$patch" 2>/dev/null; then
        echo "== $name already applied"
    else
        git -C "$M2V" apply "$patch"
        echo "== applied $name"
    fi
done < "$PRODUCTS/bridge-patches.txt"
echo "== metal2vulkan $(git -C "$M2V" rev-parse HEAD) + $(wc -l < "$PRODUCTS/bridge-patches.txt" | tr -d ' ') bridge patches"
# The tree's own rust-toolchain.toml names the toolchain (a dated nightly); rustup installs it on first use.
(cd "$M2V" && cargo build --release -p metal2vulkan --features cli)
cp "$M2V/target/release/metal2vulkan" "$PRODUCTS/metal2vulkan"

# --- Turnip and the no-op drm-shim
shim_patch="$HERE/$(pin shader_check.mesa_shim_patch.path)"
expected="$(pin shader_check.mesa_shim_patch.sha256)"
actual="$(sha256sum "$shim_patch" | cut -d' ' -f1)"
[ "$actual" = "$expected" ] || { echo "::error::$shim_patch does not match its pinned SHA-256"; exit 1; }
if git -C "$MESA" apply --reverse --check "$shim_patch" 2>/dev/null; then
    echo "== shim patch already applied"
else
    git -C "$MESA" apply "$shim_patch"
    echo "== applied $(basename "$shim_patch") ($expected)"
fi
build="$WORK/mesa-shader-check-build"
# Mesa's configure asks the first python on PATH for mako and packaging: that must be the venv's, which has them.
export PATH="$VENV:$PATH"
# The options of UniXNU's build-turnip-darwin.py, with the kernel backend and the platform of a Linux host and the
# drm-shim tool; the Darwin-only options (tu-darwin-abi-dir, the builtin pack, the build id) are that backend's.
"$VENV/meson" setup "$build" "$MESA" --prefix="$PRODUCTS/mesa" --libdir=lib \
    -Dbuildtype=release -Db_ndebug=true \
    -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm -Dplatforms= \
    -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
    -Dllvm=disabled -Dshared-glapi=disabled -Dxmlconfig=disabled -Dshader-cache=enabled \
    -Dtools=drm-shim -Dbuild-tests=false -Dvideo-codecs= -Dvulkan-layers= \
    || { tail -n 60 "$build/meson-logs/meson-log.txt" 2>/dev/null; exit 1; }
"$VENV/ninja" -C "$build" install
ls -l "$PRODUCTS/mesa/lib/libvulkan_freedreno.so" "$PRODUCTS/mesa/lib/libfreedreno_noop_drm_shim.so"
ls "$PRODUCTS"/mesa/share/vulkan/icd.d/

{
    echo "metal2vulkan $(git -C "$M2V" rev-parse HEAD) + bridge patches:"
    sed 's/^/  /' "$PRODUCTS/bridge-patches.txt"
    echo "mesa $(git -C "$MESA" rev-parse HEAD), $(git -C "$MESA" diff HEAD --name-only | wc -l | tr -d ' ') changed files (the record, ours, the shim entry)"
    echo "unixnu $(git -C "$UNIXNU" rev-parse HEAD)"
    (cd "$M2V" && rustc --version)
    cc --version | head -n 1
} | tee "$PRODUCTS/versions.txt"
