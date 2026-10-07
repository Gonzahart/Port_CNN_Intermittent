#!/usr/bin/env python3
"""
verify_provenance.py -- prove that the ported Capuchin sources are upstream
76b6eb2 plus exactly the documented patch, and that generated files match the
manifest. Exit status 0 only if every check passes.

  1. upstream/ files match UPSTREAM.lock.json (pinned commit hashes)
  2. upstream/ + patches/capuchin-76b6eb2-apollo4.patch == src/capuchin/
     (excluding the generated header and the harness file capuchin_invoke.c)
  3. msp430/capuchin-76b6eb2-avgpool.patch applies cleanly to upstream/
  4. both generated neural_network_parameters.h match msp430/model_manifest.json
"""
import filecmp, hashlib, json, os, shutil, subprocess, sys, tempfile
APP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ok = True


def check(cond, msg):
    global ok
    print(('PASS ' if cond else 'FAIL ') + msg)
    ok &= bool(cond)


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


lock = json.load(open(os.path.join(APP, 'UPSTREAM.lock.json')))
bad = [f for f, h in lock['files'].items() if sha(os.path.join(APP, 'upstream', f)) != h]
extra = [os.path.relpath(os.path.join(r, f), os.path.join(APP, 'upstream')) for r, _, fs in os.walk(os.path.join(APP, 'upstream'))
         for f in fs if '__pycache__' not in r and os.path.relpath(os.path.join(r, f), os.path.join(APP, 'upstream')) not in lock['files']]
bad += ['unlisted:' + e for e in extra]
check(not bad, 'upstream/ matches pinned hashes (%d files)%s' % (len(lock['files']), (': ' + ', '.join(bad)) if bad else ''))

with tempfile.TemporaryDirectory() as td:
    shutil.copytree(os.path.join(APP, 'upstream'), os.path.join(td, 'u'))
    r = subprocess.run(['patch', '-p1', '-s', '-i', os.path.join(APP, 'patches', 'capuchin-76b6eb2-apollo4.patch')],
                       cwd=os.path.join(td, 'u'), capture_output=True, text=True)
    check(r.returncode == 0, 'apollo4 patch applies to upstream' + (': ' + r.stdout + r.stderr if r.returncode else ''))
    diffs = []
    for d in ('decoder', 'layers', 'math', 'utils'):
        a = os.path.join(td, 'u', 'capuchin-MCU', d); b = os.path.join(APP, 'src', 'capuchin', d)
        for f in sorted(set(os.listdir(a)) | set(os.listdir(b))):
            pa, pb = os.path.join(a, f), os.path.join(b, f)
            if not (os.path.exists(pa) and os.path.exists(pb) and filecmp.cmp(pa, pb, shallow=False)):
                diffs.append(d + '/' + f)
    check(not diffs, 'src/capuchin == upstream + patch' + (' (differs: %s)' % ', '.join(diffs) if diffs else ''))

    shutil.rmtree(os.path.join(td, 'u'))
    shutil.copytree(os.path.join(APP, 'upstream'), os.path.join(td, 'u'))
    r = subprocess.run(['patch', '-p1', '-s', '--dry-run', '-i', os.path.join(APP, 'msp430', 'capuchin-76b6eb2-avgpool.patch')],
                       cwd=os.path.join(td, 'u'), capture_output=True, text=True)
    check(r.returncode == 0, 'msp430 avgpool patch applies to upstream')

man = json.load(open(os.path.join(APP, 'msp430', 'model_manifest.json')))
check(sha(os.path.join(APP, 'msp430', 'neural_network_parameters.h')) == man['msp430_header_sha256'], 'msp430 header matches manifest')
check(sha(os.path.join(APP, 'src', 'capuchin', 'neural_network_parameters.h')) == man['apollo4_header_sha256'], 'apollo4 header matches manifest')
sys.exit(0 if ok else 1)
