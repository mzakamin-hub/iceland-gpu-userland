#!/usr/bin/env bash
# Record what the platform's own codesign reports for one image or bundle: the display, the entitlements and a strict
# verification. The text is an artifact so the signature can be re-checked off the runner with a second parser.
#
#   capture-signature.sh <path> <output-directory>
set -euo pipefail
target="$1"
out="$2"
name="$(basename "$target")"
mkdir -p "$out"
{
    echo "# codesign --display --verbose=4 $name"
    codesign --display --verbose=4 "$target" 2>&1 || true
    echo
    echo "# codesign -d --entitlements :- $name"
    codesign -d --entitlements :- "$target" 2>&1 || true
    echo
    echo "# codesign --verify --strict --verbose=2 $name"
    if codesign --verify --strict --verbose=2 "$target" 2>&1; then
        echo "VERIFY: PASS"
    else
        echo "VERIFY: FAIL"
        exit 1
    fi
} | tee "$out/codesign-${name}.txt"
