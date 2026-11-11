#!/usr/bin/env python3
"""Write the build manifest: the exact pins, the Xcode and SDK that built the artifacts, and each file's SHA-256.

    make-manifest.py --stage c1 --out manifest.json FILE_OR_DIRECTORY ...
"""
import argparse
import datetime
import hashlib
import json
import os
import platform
import subprocess
import sys
from pathlib import Path


def hash_tree(path):
    """[(relative name, bytes, sha256)] for a file, or recursively for a directory."""
    root = Path(path)
    if root.is_file():
        return [(root.name, root.stat().st_size, hashlib.sha256(root.read_bytes()).hexdigest())]
    rows = []
    for name in sorted(p for p in root.rglob('*') if p.is_file()):
        rows.append((str(name.relative_to(root.parent)), name.stat().st_size,
                     hashlib.sha256(name.read_bytes()).hexdigest()))
    return rows


def tool(*command):
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=60).stdout.strip()
    except Exception as error:                                    # noqa: BLE001 - a missing tool is a fact to record
        return 'unavailable: %s' % error


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--stage', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--pins', default=str(Path(__file__).resolve().parents[1] / 'pins/inputs.json'))
    ap.add_argument('files', nargs='+')
    args = ap.parse_args()
    pins = json.loads(Path(args.pins).read_text())
    developer = os.environ.get('DEVELOPER_DIR', '')
    manifest = {
        'stage': args.stage,
        'generated': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'runner': os.environ.get('RUNNER_OS', '') + ' ' + platform.machine(),
        'target': pins['target'],
        'pins': pins,
        'xcode': {
            'DEVELOPER_DIR': developer,
            'version': tool('xcodebuild', '-version') if developer else 'not selected',
            'sdk_path': os.environ.get('SDK_PATH', ''),
            'sdk_version': os.environ.get('SDK_VERSION', ''),
            'clang': tool('xcrun', 'clang', '--version'),
        },
        'python': sys.version,
        'files': {},
    }
    for path in args.files:
        for name, size, digest in hash_tree(path):
            manifest['files'][name] = {'bytes': size, 'sha256': digest}
    Path(args.out).write_text(json.dumps(manifest, indent=2) + '\n')
    print('wrote', args.out, 'with', len(manifest['files']), 'files')


if __name__ == '__main__':
    main()
