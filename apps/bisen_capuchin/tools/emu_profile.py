#!/usr/bin/env python3
"""
emu_profile.py -- executed-instruction breakdown of one Capuchin inference in
the Cortex-M4 emulator, per decoded layer, using a CAPUCHIN_LAYER_PROFILE=1
image (the [PORT H1] hook marks layer ends). Also counts instructions spent in
memcpy, which is where the MSP430's DMA copies ([PORT P1]) land on Apollo4.

Instruction counts are NOT cycles; use them to explain board results, never as
results. The hook is installed before any code runs (Unicorn does not
instrument blocks translated before a hook exists).
"""
import argparse, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import numpy as np
from unicorn import UC_HOOK_CODE
import emu_check as ec

NAMES = ['conv1', 'avgpool1', 'conv2', 'avgpool2', 'flatten', 'fc1', 'fc2', 'fc3', 'tail']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('axf')
    ap.add_argument('--vector', type=int, default=0)
    a = ap.parse_args()
    img = ec.Image(a.axf)
    syms = {s.name: (s['st_value'] & ~1, s['st_size']) for s in img.elf.get_section_by_name('.symtab').iter_symbols()
            if s.name in ('memcpy', 'capuchin_port_layer_end')}
    if 'capuchin_port_layer_end' not in syms:
        raise SystemExit('image was not built with CAPUCHIN_LAYER_PROFILE=1')
    hook_addr = syms['capuchin_port_layer_end'][0]
    mc0, mcs = syms.get('memcpy', (0, 0))
    vh = open(os.path.join(os.path.dirname(HERE), 'src', 'bench_vectors.h')).read()
    g = lambda n, dt: np.array([int(v) for v in re.search(r'%s\[\d+\] = \{(.*?)\};' % n, vh, re.S).group(1)
                                .replace('\n', '').split(',') if v.strip()], dt)
    pix = g('BENCH_PIX', np.uint8).reshape(-1, 1024); tab = g('PIX_TO_Q10', np.int16)
    st = {'n': 0, 'mc': 0, 'on': False, 'marks': [], 'mc_marks': []}

    def cb(uc, addr, size, ud):
        if not st['on']:
            return
        st['n'] += 1
        if mc0 <= addr < mc0 + mcs:
            st['mc'] += 1
        if addr == hook_addr:
            st['marks'].append(st['n']); st['mc_marks'].append(st['mc'])
    img.uc.hook_add(UC_HOOK_CODE, cb)
    img.uc.mem_write(img.scratch, tab[pix[a.vector]].astype('<i2').tobytes())
    img.call('capuchin_load_input', img.scratch)
    st['on'] = True
    img.call('capuchin_infer', img.scratch + 0x1000)
    tot = np.diff([0] + st['marks'] + [st['n']]); mc = np.diff([0] + st['mc_marks'] + [st['mc']])
    print('%s\n| layer | instructions | of which memcpy |' % a.axf)
    for nm, t, m in zip(NAMES, tot, mc):
        print('| %s | %.3f M | %.3f M |' % (nm, t / 1e6, m / 1e6))
    print('| total | %.3f M | %.3f M |' % (st['n'] / 1e6, st['mc'] / 1e6))


if __name__ == '__main__':
    main()
