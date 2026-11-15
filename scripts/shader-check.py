#!/usr/bin/env python3
"""Run our test shaders through the two compilers the tablet's Metal path uses, with no GPU and no Apple file.

    shader-check.py --air DIR --translator BIN --profile FILE --pipeline-check BIN [--preload SHIM] --out DIR
                    [--golden DIR] [--no-driver]

For every case of shaders/cases.json (one vertex and one fragment function of one of our .metal files, compiled to AIR
by the macOS job; the .air files are run artifacts, never committed):

1. the pinned metal2vulkan translates the pair for the device the profile describes. A refusal ("FALLBACK", exit 2)
   fails the case, as does spirv-val rejecting a module (the translator runs it on every module it writes);
2. each module is disassembled (spirv-dis) and must match the case's spirv_must_match patterns; a case marked "fetch"
   must read its colour attachment (an input attachment or a storage image), not ignore it;
3. pipeline-check creates the pipeline on the driver the environment selects: on the CI job that is Turnip on Mesa's
   no-op drm-shim as the Adreno 840, so the modules go through spirv_to_nir and the ir3 compiler. A refused pipeline
   fails the case. IR3_SHADER_DEBUG=vs,fs,disasm is set so the driver's own listing lands in <case>.driver.txt;
4. with --golden DIR, DIR/<case>.json ({"vert.spv": sha256, "frag.spv": sha256}) must match the modules: a
   difference is a mismatch and fails the case. A case without a golden file is reported "unpinned", not failed, and
   the report carries the hashes to pin.

Exit 0 only when every case passes. report.json and report.txt are written to --out with everything else.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    return subprocess.run([str(c) for c in command], capture_output=True, text=True, errors='replace', **kwargs)


def check_case(case, spec, args):
    name = case['name']
    out = args.out / name
    out.mkdir(parents=True, exist_ok=True)
    result = dict(name=name, status='PASS', notes=[], sha256={})

    def fail(why):
        result['status'] = 'FAIL'
        result['notes'].append(why)
        return result

    air = args.air / (Path(case['source']).stem + '.air')
    if not air.is_file():
        return fail('no AIR for %s (the macOS job did not produce %s)' % (case['source'], air.name))
    command = [args.translator, air, out / (name + '.spv'), '--profile', args.profile,
               '--function', case.get('vertex', spec['vertex']), '--fragment', case['fragment'],
               '--emit-meta', out / (name + '.meta.json')]
    for index, fmt in enumerate(case['colors']):
        command += ['--color-format', '%d:%s' % (index, fmt)]
    env = dict(os.environ, METAL2VULKAN_REPRO_DIR=str(out / 'repro'))
    translated = run(command, env=env)
    (out / 'translator.txt').write_text('$ %s\n%s%s\nexit %d\n' % (' '.join(map(str, command)), translated.stdout,
                                                                 translated.stderr, translated.returncode))
    refused = translated.returncode != 0 or 'PASS' not in translated.stdout
    expect = case.get('expect', 'pass')
    if expect == 'refuse':
        # A case that documents a known refusal: any other outcome, or another reason, is the mismatch.
        wanted = case['refusal_match']
        if not refused:
            return fail('expected the refusal /%s/, but the translator accepted it' % wanted)
        if not re.search(wanted, translated.stderr):
            return fail('refused, but not as /%s/: %s' % (wanted, translated.stderr.strip().splitlines()[-1:]))
        result['notes'].append('refused as expected')
        return result
    if refused:
        lines = [l for l in translated.stderr.splitlines() if l.startswith('metal2vulkan:')]
        return fail('translator refused: ' + ' | '.join(lines[-3:]))
    if 'spirv-val' not in translated.stdout or 'not found' in translated.stdout:
        return fail('spirv-val did not run: ' + translated.stdout.strip())

    modules = {stage: out / ('%s.%s.spv' % (name, stage)) for stage in ('vert', 'frag')}
    listing = {}
    for stage, path in modules.items():
        if not path.is_file():
            return fail('the translator wrote no %s' % path.name)
        result['sha256'][stage + '.spv'] = hashlib.sha256(path.read_bytes()).hexdigest()
        dis = run(['spirv-dis', '--no-color', path])
        if dis.returncode:
            return fail('spirv-dis failed on %s: %s' % (path.name, dis.stderr.strip()))
        (out / (path.name + '.txt')).write_text(dis.stdout)
        listing[stage] = dis.stdout
    for pattern in case.get('spirv_must_match', []):
        if not re.search(pattern, listing['frag']):
            fail('the fragment module does not match /%s/' % pattern)
    if case.get('fetch') and not re.search(r'SubpassData|OpImageRead|OpImageTexelPointer', listing['frag']):
        fail('a framebuffer-fetch case whose fragment module reads no attachment')

    if not args.no_driver:
        command = [args.pipeline_check, '--vert', modules['vert'], '--frag', modules['frag']]
        for fmt in case['colors']:
            command += ['--color', fmt]
        env = dict(os.environ, IR3_SHADER_DEBUG='vs,fs,disasm', MESA_SHADER_CACHE_DISABLE='true')
        if args.preload:
            # The drm-shim is preloaded into this one process only, never into the translator or the tools.
            env['LD_PRELOAD'] = str(args.preload)
        driver = run(command, env=env, timeout=300)
        (out / (name + '.driver.txt')).write_text('$ %s\n%s\n--- stderr\n%s\nexit %d\n' % (
            ' '.join(map(str, command)), driver.stdout, driver.stderr, driver.returncode))
        if driver.returncode != 0 or 'PASS' not in driver.stdout:
            last = (driver.stdout.strip().splitlines() or driver.stderr.strip().splitlines() or ['no output'])[-1]
            fail('the driver refused the pipeline (exit %d): %s' % (driver.returncode, last))
        else:
            device = next((l for l in driver.stdout.splitlines() if l.startswith('device:')), '')
            result['device'] = device
            for pattern in case.get('driver_must_match', []):
                if not re.search(pattern, driver.stderr + driver.stdout):
                    fail('the driver listing does not match /%s/' % pattern)

    if args.golden:
        golden = args.golden / (name + '.json')
        if golden.is_file():
            pinned = json.loads(golden.read_text())
            for key, digest in result['sha256'].items():
                if pinned.get(key) != digest:
                    fail('mismatch: %s is %s, the golden file says %s' % (key, digest[:16], str(pinned.get(key))[:16]))
        else:
            result['notes'].append('unpinned (no golden file)')
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--air', type=Path, required=True)
    ap.add_argument('--translator', type=Path, required=True)
    ap.add_argument('--profile', type=Path, required=True)
    ap.add_argument('--pipeline-check', type=Path)
    ap.add_argument('--preload', type=Path, help='a drm-shim library to LD_PRELOAD into pipeline-check')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--golden', type=Path)
    ap.add_argument('--no-driver', action='store_true', help='translator and SPIR-V checks only')
    ap.add_argument('--cases', type=Path, default=HERE / 'shaders/cases.json')
    args = ap.parse_args()
    if not args.no_driver and not args.pipeline_check:
        ap.error('--pipeline-check is required unless --no-driver')
    # Absolute paths: a bare name would be looked up on PATH, and the driver run changes nothing about the directory.
    args.translator = args.translator.resolve()
    args.pipeline_check = args.pipeline_check.resolve() if args.pipeline_check else None
    args.preload = args.preload.resolve() if args.preload else None
    spec = json.loads(args.cases.read_text())
    args.out.mkdir(parents=True, exist_ok=True)
    forbidden = [p.name for p in args.air.iterdir() if p.suffix in ('.metallib', '.mtlb')] if args.air.is_dir() else []
    if forbidden:
        sys.exit('refusing to run: metallib files in the AIR folder: %s' % ', '.join(forbidden))
    results = [check_case(case, spec, args) for case in spec['cases']]
    lines = ['%-24s %-5s %s' % (r['name'], r['status'], '; '.join(r['notes'])) for r in results]
    failed = [r for r in results if r['status'] != 'PASS']
    devices = sorted({r['device'] for r in results if r.get('device')})
    summary = '%d cases, %d passed, %d failed' % (len(results), len(results) - len(failed), len(failed))
    text = '\n'.join(lines + devices + [summary]) + '\n'
    (args.out / 'report.txt').write_text(text)
    (args.out / 'report.json').write_text(json.dumps(dict(results=results, summary=summary), indent=2) + '\n')
    print(text, end='')
    for r in failed:
        print('::error::%s: %s' % (r['name'], '; '.join(r['notes'])))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
