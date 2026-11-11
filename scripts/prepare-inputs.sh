#!/usr/bin/env bash
# Fetch the pinned public sources UniXNU's build scripts read, by exact commit, into ./unixnu.
#
# Nothing here is a copy of anybody's tree: each source is fetched from its public repository at the commit that
# pins/inputs.json names, and every fetch is verified by `git rev-parse HEAD`. The Mesa work tree is then the pin plus
# exactly the recorded patches from UniXNU's own scripts/data/adreno-compiler-inputs.json (the list the build script
# validates), so no local Mesa patch is invented or left out.
#
# Components: unixnu (checkout + the four GPU submodules), mesa, metal2vulkan, venv (meson/ninja/mako/pyyaml).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PINS="$HERE/pins/inputs.json"
WORK="${GITHUB_WORKSPACE:?GITHUB_WORKSPACE must be set}"
UNIXNU="$WORK/unixnu"
CACHE="$UNIXNU/.cache"

read_pin() {
    python3 - "$PINS" "$1" <<'PY'
import json, sys
value = json.load(open(sys.argv[1]))
for key in sys.argv[2].split('.'):
    value = value[key]
print(value)
PY
}

# Every destructive path below is absolute and inside the workspace; refuse anything else (briefing rule 2).
inside_workspace() {
    case "$1" in
        "$WORK"/*) return 0 ;;
        *) echo "::error::refusing to write outside the workspace: $1"; exit 1 ;;
    esac
}

fetch_git() {
    local url="$1" commit="$2" dir="$3"
    inside_workspace "$dir"
    if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" = "$commit" ]; then
        echo "== $dir already at $commit"
        return 0
    fi
    rm -rf "$dir"
    mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$url"
    git -C "$dir" -c protocol.version=2 fetch -q --depth 1 origin "$commit"
    git -C "$dir" checkout -q FETCH_HEAD
    local head
    head="$(git -C "$dir" rev-parse HEAD)"
    if [ "$head" != "$commit" ]; then
        echo "::error::$dir is at $head, not the pinned $commit"
        exit 1
    fi
    echo "== $dir at $commit ($url)"
}

unixnu() {
    fetch_git "$(read_pin unixnu.repository)" "$(read_pin unixnu.commit)" "$UNIXNU"
    local path commit head
    for path in drivers/AdrenoMetal drivers/Adreno740 kits/AdrenoPM4 kits/AdrenoFirmware; do
        commit="$(read_pin "unixnu.submodules.$path")"
        git -C "$UNIXNU" submodule update --init --depth 1 --filter=blob:none "$path"
        head="$(git -C "$UNIXNU/$path" rev-parse HEAD)"
        if [ "$head" != "$commit" ]; then
            echo "::error::$path is at $head, not the pinned $commit"
            exit 1
        fi
        echo "== $path at $commit"
    done
}

mesa() {
    local dir="$CACHE/mesa-work" commit ml
    commit="$(read_pin mesa.commit)"
    # A full depth-1 checkout of the pin (118 MB pack, 514 MB tree): Mesa's meson build reaches nearly every subdir, and
    # UniXNU records sparse_directories as build *inputs*, not as a checkout that meson can configure.
    fetch_git "$(read_pin mesa.mirror)" "$commit" "$dir"
    python3 - "$UNIXNU/scripts/data/adreno-compiler-inputs.json" "$UNIXNU" "$dir" <<'PY'
import json, subprocess, sys
record, unixnu, work = json.load(open(sys.argv[1])), sys.argv[2], sys.argv[3]
for patch in record['patches']:
    subprocess.run(['git', '-C', work, 'apply', unixnu + '/' + patch], check=True)
print('== applied %d recorded Mesa patches' % len(record['patches']))
PY
    ml="$(git -C "$dir" rev-parse HEAD)"
    if [ "$ml" != "$commit" ]; then
        echo "::error::Mesa is at $ml, not the pinned $commit"
        exit 1
    fi
    echo "== Mesa $commit, $(git -C "$dir" diff HEAD --name-only | wc -l | tr -d ' ') changed files"
}

metal2vulkan() {
    fetch_git "$(read_pin translator.repository)" "$(read_pin translator.commit)" "$CACHE/gpu-reference/metal2vulkan"
}

venv() {
    python3 -m venv "$CACHE/mesa-venv"
    "$CACHE/mesa-venv/bin/python" -m pip install --disable-pip-version-check --quiet meson ninja mako pyyaml
    mkdir -p "$CACHE/mesa-tools/bin"
    "$CACHE/mesa-venv/bin/meson" --version
    "$CACHE/mesa-venv/bin/ninja" --version
}

components=("$@")
[ ${#components[@]} -gt 0 ] || components=(unixnu mesa venv)
for component in "${components[@]}"; do
    "$component"
done
df -h "$WORK" | tail -n 1
