#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 the Tango authors
"""vproc_crosscheck.py -- the firmware's video/audio processing transfers (bl616/tango/tango_vproc.c, through the host
tool vproc_stream) against sys-tango's build_tools/vproc_preset.py, byte for byte, for every file of a folder of MiSTer's filter files:
each scaler filter as Horizontal, as Vertical and as the Scan filter (scanline level 2), each gamma table, each shadow
mask in every mode, each audio filter and each preset.  Two implementations of Main_MiSTer's parsing (C and Python,
written separately from video.cpp / audio.cpp); a difference is a bug in one of them.
  vproc_crosscheck.py --sys <sys-tango> --filters <filters folder> [--quick]
Exit 0 only if every case is equal."""
import argparse, os, subprocess, sys, tempfile

ap = argparse.ArgumentParser()
ap.add_argument('--sys', required=True); ap.add_argument('--filters', required=True)
ap.add_argument('--quick', action='store_true', help='every 7th file only')
a = ap.parse_args()
sys.path.insert(0, os.path.join(a.sys, 'build_tools'))
import vproc_preset as VP

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, 'vproc_stream')
root = a.filters

def files(sub, ext):
    base = VP.folder(root, sub); out = []
    for d, ds, fs in os.walk(base):
        for f in fs:
            if f.lower().endswith(ext):
                out.append(os.path.relpath(os.path.join(d, f), base))
    out.sort()
    return out[::7] if a.quick else out

def py_stream(cfg, sl):
    xs, _ = VP.transfers(dict(cfg), root, sl)
    return b''.join(len(x).to_bytes(2, 'little') + x for x in xs)

def c_stream(cfg, sl, tmp):
    args = [TOOL, '--root', root, '--scanlines', str(sl), '--stream', tmp]
    for k, v in cfg.items():
        args += ['--' + k, v]
    r = subprocess.run(args, capture_output=True)
    if r.returncode:
        return None
    return open(tmp, 'rb').read()

cases = []
for f in files('Filters', '.txt'):
    cases.append(('H ' + f, {'hfilter': f}, 0))
    cases.append(('V ' + f, {'hfilter': 'Interpolation (Sharp).txt', 'vfilter': f}, 0))
    cases.append(('S ' + f, {'hfilter': 'Interpolation (Sharp).txt', 'sfilter': f}, 2))
for f in files('Gamma', '.txt'):
    cases.append(('G ' + f, {'gamma': f}, 0))
for f in files('Shadow_Masks', '.txt'):
    for m in ('1x', '2x', '1x rotated', '2x rotated') if not a.quick else ('2x rotated',):
        cases.append(('M %s %s' % (m, f), {'mask': f, 'maskmode': m}, 0))
for f in files('Filters_Audio', '.txt'):
    cases.append(('A ' + f, {'afilter': f}, 0))
presets = files('Presets', '.ini')
fd, tmp = tempfile.mkstemp(suffix='.bin'); os.close(fd)
bad = 0; n = 0
for name, cfg, sl in cases:
    n += 1
    p = py_stream(cfg, sl); c = c_stream(cfg, sl, tmp)
    if p != c:
        bad += 1
        if bad <= 10: print('DIFF', name, len(p), None if c is None else len(c))
for f in presets:
    for sl in (0, 2):
        n += 1
        cfg = VP.read_preset(os.path.join(VP.folder(root, 'Presets'), f))
        p = py_stream(cfg, sl)
        args = [TOOL, '--root', root, '--scanlines', str(sl), '--preset', f, '--stream', tmp]
        r = subprocess.run(args, capture_output=True)
        c = open(tmp, 'rb').read() if r.returncode == 0 else None
        if p != c:
            bad += 1
            if bad <= 10: print('DIFF preset', f, sl, len(p), None if c is None else len(c), r.stderr.decode()[:200])
os.unlink(tmp)
print('vproc_crosscheck: %s  %d cases (%d filters x H/V/Scan, %d gamma, %d masks x 4 modes, %d audio filters, %d presets x 2), %d differ'
      % ('PASS' if bad == 0 else 'FAIL', n, len(files('Filters', '.txt')), len(files('Gamma', '.txt')),
         len(files('Shadow_Masks', '.txt')), len(files('Filters_Audio', '.txt')), len(presets), bad))
sys.exit(0 if bad == 0 else 1)
