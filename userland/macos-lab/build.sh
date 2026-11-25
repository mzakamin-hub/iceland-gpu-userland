#!/usr/bin/env bash
# sheng's macOS lab probes (kit2 U5), built into $BUILD_DIR by scripts/build-userland.sh on the macOS runner with TARGET
# (arm64e-apple-macos27.0), SDK_PATH and BUILD_DIR set. The five sources are UniXNU-sheng c8f92b2 scripts/macos-lab/*,
# copied with one provenance line prepended and hash-pinned in pins/inputs.json (sources.macos_lab_*); the flags are
# sheng's own (scripts/prepare-sheng-gpu-lab.py:114-148 at that commit). Signing and packing are package-userland.py's.
#
# What each one answers on the tablet (run by hand or by a lab job; none is installed as a daemon by this component):
#   clock-probe                         the counter frequency against Darwin's clocks around one bounded sleep (icetb24)
#   video-probe FILE --require-hardware decodes FILE through VideoToolbox and fails unless the hardware decoder (QcomIris) did
#   gpu-correctness-probe --all         Metal compute/render results checked on the CPU, plus the compound cases
#   icon-probe DEST.png                 renders Settings-style icons through Core Image and Metal into DEST
#   gpu-scene                           a Core Animation scene of 20 layers asking for 120 Hz for 360 s (--describe, --reset-session)
# A/B rule (sheng audit-gpu-desktop-comparison.py): a comparison run in which WindowServer's PID changed is rejected.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

python3 - "$repo" <<'PINS'
import hashlib, json, sys
repo = sys.argv[1]
sources = json.load(open(repo + '/pins/inputs.json'))['sources']
lab = {k: v for k, v in sources.items() if k.startswith('macos_lab_')}
assert len(lab) == 5, 'five pinned lab sources expected, found %d' % len(lab)
for name, pin in lab.items():
    data = open(repo + '/' + pin['path'], 'rb').read()
    assert hashlib.sha256(data).hexdigest() == pin['sha256'], pin['path'] + ' differs from its pin'
    body = data.split(b'\n', 1)[1]
    assert hashlib.sha256(body).hexdigest() == pin['upstream_sha256'], pin['path'] + ' differs from ' + pin['upstream_path']
print('== the five lab sources match their pins (UniXNU-sheng %s)' % next(iter(lab.values()))['commit'][:12])
PINS

probe() {
    local name="$1" frameworks="$2" extra="$3" source="$4"
    local flags=() framework
    for framework in $frameworks; do flags+=(-framework "$framework"); done
    # shellcheck disable=SC2086
    xcrun --sdk macosx clang -target "$TARGET" -isysroot "$SDK_PATH" -O2 -Wall -Wextra -Werror $extra \
        ${flags[@]+"${flags[@]}"} "$here/$source" -Wl,-segalign,0x4000 -o "$BUILD_DIR/$name"
    echo "== $name $(wc -c < "$BUILD_DIR/$name" | tr -d ' ') bytes"
}

probe gpu-scene "AppKit QuartzCore" "-fobjc-arc" scene.m
probe clock-probe "" "" clock-probe.c
probe icon-probe "AppKit CoreImage Metal" "-fobjc-arc" icon-probe.m
probe gpu-correctness-probe "Foundation Metal IOSurface" "-fobjc-arc" gpu-correctness-probe.m
probe video-probe "AVFoundation VideoToolbox CoreMedia CoreVideo" "-Wno-deprecated-declarations -fobjc-arc" video-probe.m
