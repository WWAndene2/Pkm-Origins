#!/usr/bin/env python3
"""Step 3c: the matching check of docs/RECOMPILATION_PLAN.md §4 for the translator's first slice. Every function of the
list that tools/arm2c.py translates is compiled natively and run against the original ARM code executed by Unicorn (an
ARM CPU emulator, the original instructions themselves) on the same random registers and memory; a function matches when
registers r0-r12, the flags and the whole test memory are equal after every trial.

  match_leaf.py <code.bin> <functions.tsv> [--trials 200] [--limit N] [--base 0x100000] [--keep DIR]

Trials where the C stops on a fault (an access out of the 64 KiB test memory, or unaligned) are skipped, not counted as
matches; a function with no valid trial is reported apart. --keep DIR writes the generated C there (outside the
repository: it holds the game's literal constants). Needs capstone, unicorn and a C compiler (cc).
Unicorn stands in for Azahar here (same ARM instructions, no 3DS system); whole-game checks use Azahar (§4).
"""
import argparse
import ctypes
import os
import random
import subprocess
import tempfile

from unicorn import UC_ARCH_ARM, UC_MODE_ARM, Uc, UcError
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                               UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP)

import arm2c

REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
        UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12]
MB, MS, RET = 0x08000000, 0x10000, 0x0FF00000

PRELUDE = r"""
#include <stdint.h>
#include <string.h>
typedef struct { uint32_t r[16]; uint32_t n, z, c, v; int fault; uint32_t steps; } Cpu;
static uint8_t* M; static uint32_t MBASE, MSIZE;
static int ok(Cpu* c, uint32_t a, uint32_t n) { if (a - MBASE > MSIZE - n || a % n) /* unsigned: an address near 0xFFFFFFFF cannot wrap past the check */ { c->fault = 1; return 0; } return 1; }
static uint32_t ld32(Cpu* c, uint32_t a) { uint32_t v = 0; if (ok(c, a, 4)) memcpy(&v, M + a - MBASE, 4); return v; }
static uint32_t ld16(Cpu* c, uint32_t a) { uint16_t v = 0; if (ok(c, a, 2)) memcpy(&v, M + a - MBASE, 2); return v; }
static uint32_t ld8(Cpu* c, uint32_t a) { return ok(c, a, 1) ? M[a - MBASE] : 0; }
static void st32(Cpu* c, uint32_t a, uint32_t v) { if (ok(c, a, 4)) memcpy(M + a - MBASE, &v, 4); }
static void st16(Cpu* c, uint32_t a, uint32_t v) { uint16_t h = v; if (ok(c, a, 2)) memcpy(M + a - MBASE, &h, 2); }
static void st8(Cpu* c, uint32_t a, uint32_t v) { if (ok(c, a, 1)) M[a - MBASE] = v; }
"""


def main():
    p = argparse.ArgumentParser()
    p.add_argument("code")
    p.add_argument("functions")
    p.add_argument("--trials", type=int, default=200)
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--base", type=lambda s: int(s, 0), default=0x100000)
    p.add_argument("--keep")
    a = p.parse_args()
    code = open(a.code, "rb").read()
    starts = sorted(int(l.split()[0], 16) for l in open(a.functions) if l.strip() and not l.startswith("#"))
    ends = starts[1:] + [a.base + len(code)]
    funcs, reasons = [], {}
    for s, e in zip(starts, ends):
        try:
            c, size = arm2c.translate(code, a.base, s, min(e, s + 0x400), "f_%08X" % s)
            funcs.append((s, c, size))
        except arm2c.Unsupported as x:
            k = str(x).split(" ")[0] if str(x).startswith("instruction") else str(x)
            k = "instruction " + str(x).split()[1] if str(x).startswith("instruction") else k
            reasons[k] = reasons.get(k, 0) + 1
        if a.limit and len(funcs) >= a.limit:
            break
    tmp = a.keep or tempfile.mkdtemp()
    os.makedirs(tmp, exist_ok=True)
    src = os.path.join(tmp, "leaf.c")
    with open(src, "w") as f:
        f.write(PRELUDE)
        for s, c, _ in funcs:
            f.write(c + "\n")
        f.write("typedef void (*Fn)(Cpu*);\nstatic const Fn T[] = {%s};\n" % ", ".join("f_%08X" % s for s, _, _ in funcs))
        f.write("void run(int i, Cpu* c, uint8_t* m, uint32_t b, uint32_t n) { M = m; MBASE = b; MSIZE = n; T[i](c); }\n")
    lib = os.path.join(tmp, "leaf.so")
    subprocess.run(["cc", "-O1", "-shared", "-fPIC", "-w", "-o", lib, src], check=True)
    so = ctypes.CDLL(lib)

    class Cpu(ctypes.Structure):
        _fields_ = [("r", ctypes.c_uint32 * 16), ("n", ctypes.c_uint32), ("z", ctypes.c_uint32), ("c", ctypes.c_uint32),
                    ("v", ctypes.c_uint32), ("fault", ctypes.c_int), ("steps", ctypes.c_uint32)]

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    size = (len(code) + 0xFFF) & ~0xFFF
    uc.mem_map(a.base, size)
    uc.mem_write(a.base, code)
    uc.mem_map(MB, MS)
    uc.mem_map(RET, 0x1000)  # the return address holds "b .": Unicorn stops there (until) even when it reaches it mid-block
    uc.mem_write(RET, bytes.fromhex("feffffea") * 1024)
    rng = random.Random(4)
    matched, mismatched, untested = 0, [], 0
    for idx, (s, _, _) in enumerate(funcs):
        valid, bad = 0, None
        for _ in range(a.trials):
            regs = [rng.choice([MB + rng.randrange(0, MS // 2, 4), rng.getrandbits(32), rng.randrange(0, 64)]) for _ in range(13)]
            mem = bytearray(rng.randbytes(MS))
            for o in range(0, MS, 4):  # half the words point into the test memory, so pointer chains stay inside it
                if rng.random() < 0.5:
                    mem[o:o + 4] = (MB + rng.randrange(0, MS - 0x400, 4)).to_bytes(4, "little")
            mem = bytes(mem)
            flags = rng.getrandbits(4)
            cpu = Cpu()
            for k in range(13):
                cpu.r[k] = regs[k]
            cpu.n, cpu.z, cpu.c, cpu.v = (flags >> 3) & 1, (flags >> 2) & 1, (flags >> 1) & 1, flags & 1
            cmem = ctypes.create_string_buffer(mem, MS)
            so.run(idx, ctypes.byref(cpu), cmem, MB, MS)
            if cpu.fault:
                continue
            valid += 1
            uc.reg_write(UC_ARM_REG_CPSR, 0x10 | (flags << 28))  # first: a mode change banks sp and lr
            for k in range(13):
                uc.reg_write(REGS[k], regs[k])
            uc.reg_write(UC_ARM_REG_SP, MB + MS - 0x100)
            uc.reg_write(UC_ARM_REG_LR, RET)
            uc.mem_write(MB, mem)
            try:
                uc.emu_start(s, RET, count=200000)
            except UcError as e:
                bad = "unicorn: %s" % e
                break
            ur = [uc.reg_read(r) for r in REGS]
            cp = uc.reg_read(UC_ARM_REG_CPSR) >> 28
            cr = [cpu.r[k] for k in range(13)]
            cf = (cpu.n << 3) | (cpu.z << 2) | (cpu.c << 1) | cpu.v
            if ur != cr:
                bad = "registers: %s" % [(k, hex(ur[k]), hex(cr[k])) for k in range(13) if ur[k] != cr[k]][:3]
            elif cp != cf:
                bad = "flags: arm %X, c %X" % (cp, cf)
            elif bytes(uc.mem_read(MB, MS)) != cmem.raw:
                bad = "memory"
            if bad:
                break
        if bad:
            mismatched.append((s, bad))
        elif valid == 0:
            untested += 1
        else:
            matched += 1
    print("%d functions in the list; %d translated (leaf subset); %d matched, %d mismatched, %d with no valid trial"
          % (len(starts), len(funcs), matched, len(mismatched), untested))
    for s, why in mismatched[:20]:
        print("  MISMATCH %08X %s" % (s, why))
    print("not translated, by first reason:")
    for k, v in sorted(reasons.items(), key=lambda kv: -kv[1])[:12]:
        print("  %6d  %s" % (v, k))


if __name__ == "__main__":
    main()
