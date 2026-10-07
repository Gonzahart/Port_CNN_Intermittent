#!/usr/bin/env python3
"""
emu_check.py -- execute the COMPILED Apollo4 image's inference functions in a
Cortex-M4 instruction-set emulator (Unicorn/QEMU, with the DSP extension) and
compare every output with the host reference embedded in bench_vectors.h.

This checks the actual ARM machine code (GCC -O3, Thumb-2, CMSIS-DSP SMLALD
path for CAPUCHIN_KERNEL=2, the RUIC engine's SIMD path) before board time. It
does NOT model Apollo4 timing (no MRAM wait states, caches or bus); the
instruction counts it can report are a sanity check, never a result.

Only pure compute functions are called (capuchin_load_input, capuchin_infer,
ruic_infer); no HAL/peripheral code runs.
"""
import argparse, os, sys
import numpy as np
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UcError
from unicorn.arm_const import *

HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
RET = 0x0FFF0000          # sentinel return address (mapped, never executed)


class Image:
    def __init__(self, axf):
        self.f = open(axf, 'rb'); self.elf = ELFFile(self.f)
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M4)
        for base, size in ((0x00000000, 0x00200000), (0x10000000, 0x00060000), (0x10060000, 0x00100000),
                           (RET & ~0xFFF, 0x1000), (0xE0000000, 0x00100000)):
            self.uc.mem_map(base, size)
        for seg in self.elf.iter_segments():
            if seg['p_type'] == 'PT_LOAD' and seg['p_filesz']:
                self.uc.mem_write(seg['p_paddr'], seg.data())
        # .data: copy load image to RAM; .bss: already zero (fresh mapping)
        for sec in self.elf.iter_sections():
            if sec.name == '.data' and sec['sh_size']:
                lma = self._lma(sec)
                self.uc.mem_write(sec['sh_addr'], bytes(self.uc.mem_read(lma, sec['sh_size'])))
        self.uc.mem_write(0xE000ED88, (0xF << 20).to_bytes(4, 'little'))   # CPACR: FPU on
        self.uc.reg_write(UC_ARM_REG_CONTROL, 0)
        self.sym = {s.name: s['st_value'] for s in self.elf.get_section_by_name('.symtab').iter_symbols() if s.name}
        self.scratch = 0x10050000            # top of TCM region, unused by the image
        self.sp = 0x1005F000

    def _lma(self, sec):
        for seg in self.elf.iter_segments():
            if seg['p_type'] == 'PT_LOAD' and seg['p_vaddr'] <= sec['sh_addr'] < seg['p_vaddr'] + seg['p_memsz']:
                return seg['p_paddr'] + (sec['sh_addr'] - seg['p_vaddr'])
        return sec['sh_addr']

    def call(self, name, *args, count=False):
        uc = self.uc
        for i, v in enumerate(args):
            uc.reg_write(UC_ARM_REG_R0 + i, v)
        uc.reg_write(UC_ARM_REG_SP, self.sp)
        uc.reg_write(UC_ARM_REG_LR, RET | 1)
        n = [0]
        h = uc.hook_add(UC_HOOK_CODE, lambda *a: n.__setitem__(0, n[0] + 1)) if count else None
        try:
            uc.emu_start(self.sym[name] | 1, RET, count=0)
        finally:
            if h is not None:
                uc.hook_del(h)
        return uc.reg_read(UC_ARM_REG_R0), n[0]

    def read(self, addr, n, dtype):
        return np.frombuffer(bytes(self.uc.mem_read(addr, n * np.dtype(dtype).itemsize)), dtype=dtype)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('axf')
    ap.add_argument('--kernel', type=int, default=1)
    ap.add_argument('--n', type=int, default=100)
    ap.add_argument('--count', type=int, default=0, help='also count instructions on the first K vectors')
    a = ap.parse_args()
    import re
    vh = open(os.path.join(os.path.dirname(HERE), 'src', 'bench_vectors.h')).read()

    def arr(name, dt):
        m = re.search(r'%s\[\d+\] = \{(.*?)\};' % name, vh, re.S)
        return np.array([int(v) for v in m.group(1).replace('\n', '').split(',') if v.strip()], dt)
    pix = arr('BENCH_PIX', np.uint8).reshape(-1, 1024); tab = arr('PIX_TO_Q10', np.int16)
    key = 'CPU' if a.kernel == 0 else 'LEA'
    exp_c = arr('BENCH_CAP_%s_SCORES' % key, np.int16).reshape(-1, 10)
    exp_r = arr('BENCH_RUIC_SCORES', np.int8).reshape(-1, 10)
    img = Image(a.axf)
    have_ruic = 'ruic_infer' in img.sym
    ok_c = ok_r = 0; ic = []; ir = []
    for v in range(a.n):
        q = tab[pix[v]].astype('<i2')
        img.uc.mem_write(img.scratch, q.tobytes())
        cnt = v < a.count
        _, n1 = img.call('capuchin_load_input', img.scratch, count=cnt)
        lab, n2 = img.call('capuchin_infer', img.scratch + 0x1000, count=cnt)
        s = img.read(img.scratch + 0x1000, 10, '<i2')
        ok_c += np.array_equal(s, exp_c[v])
        if cnt: ic.append(n1 + n2)
        if have_ruic:
            img.uc.mem_write(img.scratch + 0x2000, (pix[v].astype(np.int16) - 128).astype(np.int8).tobytes())
            lab_r, n3 = img.call('ruic_infer', img.scratch + 0x2000, img.scratch + 0x3000, count=cnt)
            r = img.read(img.scratch + 0x3000, 10, np.int8)
            ok_r += np.array_equal(r, exp_r[v])
            if cnt: ir.append(n3)
    print('%s: capuchin bit-exact %d/%d%s' % (a.axf,
          ok_c, a.n, ('; ruic bit-exact %d/%d' % (ok_r, a.n)) if have_ruic else ''))
    if ic:
        print('  instructions/inference (emulator, NOT cycles): capuchin %.0f%s' % (
            np.mean(ic), ('  ruic %.0f  ratio %.2f' % (np.mean(ir), np.mean(ic) / np.mean(ir))) if ir else ''))


if __name__ == '__main__':
    main()
