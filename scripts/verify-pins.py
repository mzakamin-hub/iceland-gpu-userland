#!/usr/bin/env python3
"""Check that every fetched source is at the commit pins/inputs.json names, and that our patches are applied.

    verify-pins.py [unixnu] [mesa] [translator]     # no argument checks all three groups
"""
import json
import subprocess
import sys
from pathlib import Path

PINS = Path(__file__).resolve().parents[1] / 'pins/inputs.json'


def git(*args):
    return subprocess.run(['git'] + list(args), capture_output=True, text=True)


def head(path):
    return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()


def main():
    pins = json.loads(PINS.read_text())
    groups = sys.argv[1:] or ['unixnu', 'mesa', 'translator']
    unknown = set(groups) - {'unixnu', 'mesa', 'translator'}
    if unknown:
        sys.exit('unknown group(s): %s' % ', '.join(sorted(unknown)))
    expected = {}
    if 'unixnu' in groups:
        expected['unixnu'] = pins['unixnu']['commit']
        for path, commit in pins['unixnu']['submodules'].items():
            expected['unixnu/' + path] = commit
    if 'mesa' in groups:
        expected['unixnu/.cache/mesa-work'] = pins['mesa']['commit']
    if 'translator' in groups:
        expected['unixnu/.cache/gpu-reference/metal2vulkan'] = pins['translator']['commit']
    missing = []
    for path, commit in expected.items():
        if not Path(path, '.git').exists():
            missing.append(path)
            continue
        actual = head(path)
        print('%-52s %s %s' % (path, actual[:12], 'ok' if actual == commit else 'DIFFERS'))
        if actual != commit:
            print('::error::%s is at %s, pinned %s' % (path, actual, commit))
            return 1
    if missing:
        print('::error::not fetched: %s' % ', '.join(missing))
        return 1
    for name, patch in (pins['patches'].items() if 'unixnu' in groups else ()):
        target = 'unixnu/' + patch['applies_to']
        result = git('-C', target, 'apply', '--reverse', '--check', str(PINS.parent.parent / patch['path']))
        applied = result.returncode == 0
        print('%-52s %s' % (patch['path'], 'applied' if applied else 'NOT APPLIED'))
        if not applied:
            print('::error::%s is not applied: %s' % (patch['path'], result.stderr.strip()))
            return 1
    print('PASS: %d checkouts at their pins, %d patches applied' % (len(expected), len(pins['patches'])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
