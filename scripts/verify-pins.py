#!/usr/bin/env python3
"""Check that every fetched source is at the commit pins/inputs.json names, before any build runs."""
import json
import subprocess
import sys
from pathlib import Path

PINS = Path(__file__).resolve().parents[1] / 'pins/inputs.json'


def head(path):
    return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()


def main():
    pins = json.loads(PINS.read_text())
    checked = []
    expected = {'unixnu': pins['unixnu']['commit']}
    for path, commit in pins['unixnu']['submodules'].items():
        expected['unixnu/' + path] = commit
    expected['unixnu/.cache/mesa-work'] = pins['mesa']['commit']
    if Path('unixnu/.cache/gpu-reference/metal2vulkan/.git').exists():
        expected['unixnu/.cache/gpu-reference/metal2vulkan'] = pins['translator']['commit']
    missing = []
    for path, commit in expected.items():
        if not Path(path, '.git').exists():
            missing.append(path)
            continue
        actual = head(path)
        status = 'ok' if actual == commit else 'DIFFERS'
        print('%-52s %s %s' % (path, actual[:12], status))
        if actual != commit:
            print('::error::%s is at %s, pinned %s' % (path, actual, commit))
            return 1
        checked.append(path)
    if missing:
        print('::error::not fetched: %s' % ', '.join(missing))
        return 1
    print('PASS: %d checkouts at their pins' % len(checked))
    return 0


if __name__ == '__main__':
    sys.exit(main())
