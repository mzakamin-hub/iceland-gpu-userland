#!/usr/bin/env python3
"""Sign ad hoc, record and pack the products of one userland component, on the macOS runner.

    package-userland.py <component-directory> <build-directory> <artifact-directory>

<component-directory>/install.json names the products its build.sh left in <build-directory>:

    {"component": "audio-hal",
     "products": [{"path": "IcelandAudio.driver",            a bundle directory or a single Mach-O file
                   "identifier": "org.iceland.audio.hal",    the code signing identifier
                   "install": "Library/Audio/Plug-Ins/HAL/IcelandAudio.driver",   where the add-on puts it (see below)
                   "executables": ["Contents/MacOS/IcelandAudio"]}]}    bundle only: its Mach-O files, main one first

A bundle that carries another bundle (an app with its system extension) names it under "nested":
    "nested": [{"path": "Contents/Library/SystemExtensions/<id>.systemextension",   relative to the product
                "identifier": "...", "entitlements": "extension.entitlements"}]    entitlements optional
Each nested bundle is signed first, as a bundle of its own (its identifier, its entitlements, its seal); its Mach-O
files still stand in the product's "executables", so the manifest records them like every other.

Every Mach-O gets the signature the kernel's trust needs and nothing more: ad hoc (no identity, no key, no
certificate), 4 KiB pages, the given identifier, no entitlements unless the product names an "entitlements" plist of
ours. That is the signature adrenoprobed and the Metal plugin already carry. The owner key is never here: the owner
trust entry is made on the project machine from the CDHash this script records.

"install" is a path of the boot add-on (NGPUADD2): "Library/..." lands under the Data volume's /Library, anything
else under /private/var/ngpu/. The local packer reads it from the manifest; nothing is installed here.

Artifact: <name>.tar.gz for a bundle or the file itself, codesign-*.txt for each Mach-O, and userland-manifest.json
(install paths, modes, SHA-256 and CDHash of every Mach-O, the toolchain).
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
MACHO = (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf')


def run(*command):
    return subprocess.run([str(c) for c in command], check=True, capture_output=True, text=True)


def sign(target, identifier, entitlements):
    command = ['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', identifier]
    if entitlements:
        command += ['--entitlements', entitlements]
    run(*command, target)
    run('codesign', '--verify', '--strict', target)


def cdhash(target):
    shown = subprocess.run(['codesign', '--display', '--verbose=4', str(target)], capture_output=True, text=True)
    found = re.search(r'^CDHash=([0-9a-f]{40})$', shown.stderr + shown.stdout, re.M)
    if not found:
        sys.exit('no CDHash in codesign\'s display of %s' % target)
    return found.group(1)


def main():
    component, build, out = (Path(p) for p in sys.argv[1:4])
    spec = json.loads((component / 'install.json').read_text())
    out.mkdir(parents=True, exist_ok=True)
    manifest = dict(component=spec['component'], target='arm64e-apple-macos27.0', products=[],
                    xcode=os.environ.get('XCODE_APP', ''), sdk=os.environ.get('SDK_VERSION', ''),
                    signature='ad hoc, 4 KiB pages, no key')
    for product in spec['products']:
        path = build / product['path']
        if not path.exists():
            sys.exit('%s: build.sh left no %s' % (spec['component'], product['path']))
        install = product['install']
        if install.startswith('/') or '..' in install.split('/'):
            sys.exit('%s: install path must be relative, without ".."' % install)
        entitlements = component / product['entitlements'] if product.get('entitlements') else None
        record = dict(path=product['path'], identifier=product['identifier'], install=install, files=[])
        if path.is_dir():
            executables = product['executables']
            # Nested bundles first, then loose inner Mach-O files, the bundle (its main executable and its seal) last.
            nested = product.get('nested', [])
            for item in nested:
                if item['path'].startswith('/') or '..' in item['path'].split('/') or not (path / item['path']).is_dir():
                    sys.exit('%s: nested path must be a folder inside the product' % item['path'])
                sign(path / item['path'], item['identifier'],
                     component / item['entitlements'] if item.get('entitlements') else None)
            inside = tuple(item['path'].rstrip('/') + '/' for item in nested)
            for relative in executables[1:]:
                if not relative.startswith(inside):
                    sign(path / relative, product['identifier'] + '.' + Path(relative).name, None)
            sign(path, product['identifier'], entitlements)
            for item in sorted(p for p in path.rglob('*') if p.is_file()):
                relative = item.relative_to(path).as_posix()
                is_macho = item.read_bytes()[:4] in MACHO
                if is_macho and relative not in executables:
                    sys.exit('%s holds a Mach-O the install.json does not list: %s' % (product['path'], relative))
                entry = dict(install=install + '/' + relative, mode='0755' if is_macho else '0644',
                             sha256=hashlib.sha256(item.read_bytes()).hexdigest(), bytes=item.stat().st_size)
                if is_macho:
                    entry['cdhash'] = cdhash(item)
                    subprocess.run(['bash', str(HERE / 'scripts/capture-signature.sh'), str(item), str(out)], check=True)
                if len(entry['install']) > 159:
                    sys.exit('%s: longer than the add-on\'s 159-byte path limit' % entry['install'])
                record['files'].append(entry)
            archive = out / (path.name + '.tar.gz')
            run('tar', '-czf', archive, '-C', path.parent, path.name)
            record['archive'] = archive.name
        else:
            sign(path, product['identifier'], entitlements)
            subprocess.run(['bash', str(HERE / 'scripts/capture-signature.sh'), str(path), str(out)], check=True)
            shutil.copy2(path, out / path.name)
            record['files'].append(dict(install=install, mode='0755', sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                        bytes=path.stat().st_size, cdhash=cdhash(path)))
            record['archive'] = None
        manifest['products'].append(record)
        for entry in record['files']:
            print('%s %s %s %s' % (entry['mode'], entry['sha256'][:16], entry.get('cdhash', '-' * 40), entry['install']))
    (out / 'userland-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('PASS: %s, %d product(s)' % (spec['component'], len(manifest['products'])))


if __name__ == '__main__':
    main()
