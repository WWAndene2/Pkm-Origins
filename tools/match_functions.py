#!/usr/bin/env python3
"""The matching check of docs/RECOMPILATION_PLAN.md §4 for the translator (steps 3 and 5). Every function of the list that
tools/arm2c.py translates, and whose callees it translates too, is compiled natively and run against the original ARM
code executed by Unicorn (an ARM CPU emulator, the original instructions themselves) on the same random registers and
memory, the stack inside it, over the game's own memory from a running game with --ram (writes to it logged on both
sides, compared, then undone); a function matches when registers r0-r12 and sp, the flags and the whole test memory are
equal after every trial. Its calls run in both: the original callee in Unicorn, the translated one in C.

  match_functions.py <code.bin> <functions.tsv> [--trials 200] [--limit N] [--base 0x100000] [--keep DIR]

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

from unicorn import UC_ARCH_ARM, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc, UcError
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                               UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP,
                               UC_ARM_REG_PC)

import arm2c

REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
        UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12]
# the random test memory and the return address, away from the game's own memory (heap 0x08000000, stack below
# 0x10000000, linear heap 0x14000000)
MB, MS, RET = 0x1F000000, 0x10000, 0x1E000000
# --ram: the game's memory from a running game (Pomegrade's headless Azahar, "dump ADDRESS LENGTH FILE"), the files at
# these addresses
RAM = (("code.bin", 0x00100000), ("heap.bin", 0x08000000), ("linear.bin", 0x14000000), ("stack.bin", 0x0FF00000))

PRELUDE = r"""
#include <stdint.h>
#include <string.h>
typedef struct { uint32_t r[16]; uint32_t n, z, c, v; int fault; uint32_t steps; } Cpu;
/* memory regions: 0 is the random test memory, the others the game's memory (--ram), whose writes are logged (address,
   old byte) so that a trial can be compared on what it wrote and undone after */
static uint8_t* RP[8]; static uint32_t RB[8], RS[8]; static int NR;
static uint32_t* LA; static uint8_t* LV; static int NL, LMAX;
static uint8_t* at(Cpu* c, uint32_t a, uint32_t n, int w)
{
    if (a % n) { c->fault = 1; return 0; }
    for (int i = 0; i < NR; i++)
        if (a - RB[i] <= RS[i] - n) { /* unsigned: an address near 0xFFFFFFFF cannot wrap past the check */
            uint8_t* p = RP[i] + (a - RB[i]);
            if (w && i > 0)
                for (uint32_t k = 0; k < n; k++) {
                    if (NL == LMAX) { c->fault = 6; return 0; }
                    LA[NL] = a + k; LV[NL] = p[k]; NL++;
                }
            return p;
        }
    c->fault = 1;
    return 0;
}
static uint32_t ld32(Cpu* c, uint32_t a) { uint32_t v = 0; uint8_t* p = at(c, a, 4, 0); if (p) memcpy(&v, p, 4); return v; }
static uint32_t ld16(Cpu* c, uint32_t a) { uint16_t v = 0; uint8_t* p = at(c, a, 2, 0); if (p) memcpy(&v, p, 2); return v; }
static uint32_t ld8(Cpu* c, uint32_t a) { uint8_t* p = at(c, a, 1, 0); return p ? *p : 0; }
static void st32(Cpu* c, uint32_t a, uint32_t v) { uint8_t* p = at(c, a, 4, 1); if (p) memcpy(p, &v, 4); }
static void st16(Cpu* c, uint32_t a, uint32_t v) { uint16_t h = v; uint8_t* p = at(c, a, 2, 1); if (p) memcpy(p, &h, 2); }
static void st8(Cpu* c, uint32_t a, uint32_t v) { uint8_t* p = at(c, a, 1, 1); if (p) *p = v; }
void setup(int nr, uint8_t** p, uint32_t* b, uint32_t* s, uint32_t* la, uint8_t* lv, int lmax)
{ NR = nr; for (int i = 0; i < nr; i++) { RP[i] = p[i]; RB[i] = b[i]; RS[i] = s[i]; } LA = la; LV = lv; LMAX = lmax; }
void scratch(uint8_t* p) { RP[0] = p; NL = 0; }
int logged(void) { return NL; }
uint8_t* where(uint32_t a) { for (int i = 1; i < NR; i++) if (a - RB[i] < RS[i]) return RP[i] + (a - RB[i]); return 0; }
void undo(void) { while (NL) { NL--; *where(LA[NL]) = LV[NL]; } }
"""


def main():
    p = argparse.ArgumentParser()
    p.add_argument("code")
    p.add_argument("functions")
    p.add_argument("--trials", type=int, default=200)
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--base", type=lambda s: int(s, 0), default=0x100000)
    p.add_argument("--keep")
    p.add_argument("--cro", help="the game's modules (static.crs): the words of the program they rewrite are read when run")
    p.add_argument("--only", type=lambda v: int(v, 16), help="test this function only (and what it calls)")
    p.add_argument("--ram", help="a folder of the game's memory (code.bin, heap.bin, linear.bin, stack.bin) under the tests")
    a = p.parse_args()
    code = open(a.code, "rb").read()
    rows = [l.split() for l in open(a.functions) if l.strip() and not l.startswith("#")]
    starts = sorted(int(r[0], 16) for r in rows)
    thumb = {int(r[0], 16) for r in rows if r[3] == "thumb"}  # bounds of their neighbours; arm2c reads ARM only
    size = {int(r[0], 16): int(r[4]) for r in rows}
    arm = frozenset(x for x in starts if x not in thumb)
    relocated = arm2c.static_relocations(a.cro) if a.cro else frozenset()
    funcs, reasons, deps = {}, {}, {}
    for s in starts:
        if s in thumb:
            reasons["thumb"] = reasons.get("thumb", 0) + 1
            continue
        try:
            callees = set()
            c, n = arm2c.translate(code, a.base, s, s + min(size[s] or 0x400, 0x4000), "f_%08X" % s, arm, callees, relocated)
            funcs[s], deps[s] = (c, n), callees
        except arm2c.Unsupported as x:
            k = str(x).split(" ")[0] if str(x).startswith("instruction") else str(x)
            k = "instruction " + str(x).split()[1] if str(x).startswith("instruction") else k
            reasons[k] = reasons.get(k, 0) + 1
    # a function whose callee is not translated is not either, until none is left
    while True:
        drop = [s for s in funcs if not deps[s] <= funcs.keys()]
        if not drop:
            break
        for s in drop:
            del funcs[s]
            reasons["calls an untranslated function"] = reasons.get("calls an untranslated function", 0) + 1
    if a.limit or a.only:  # the first N (or the one asked), and every function they call (directly or not)
        keep, todo = set(), [a.only] if a.only else sorted(funcs)[:a.limit]
        while todo:
            k = todo.pop()
            if k not in keep:
                keep.add(k)
                todo.extend(deps[k])
        funcs = {k: v for k, v in funcs.items() if k in keep}
    with_calls = sum(1 for s in funcs if deps[s])
    funcs = [(s, c, n) for s, (c, n) in sorted(funcs.items())]
    tmp = a.keep or tempfile.mkdtemp()
    os.makedirs(tmp, exist_ok=True)
    src = os.path.join(tmp, "functions.c")
    with open(src, "w") as f:
        f.write(PRELUDE)
        f.write(arm2c.RUNTIME)
        for s, _, _ in funcs:
            f.write("void f_%08X(Cpu* c, uint32_t ret);\n" % s)
        for s, c, _ in funcs:
            f.write(c + "\n")
        # call_indirect: a binary search of the translated functions by address (an untranslated target: fault 4,
        # the trial does not count)
        f.write("static const uint32_t A[] = {%s};\n" % ", ".join("0x%Xu" % s for s, _, _ in funcs))
        f.write("typedef void (*Fn)(Cpu*, uint32_t);\nstatic const Fn T[] = {%s};\n" % ", ".join("f_%08X" % s for s, _, _ in funcs))
        f.write("void run(int i, Cpu* c) { T[i](c, c->r[14]); }\n")
        f.write("void call_indirect(Cpu* c, uint32_t t, uint32_t ret) { int lo = 0, hi = %d;"
                " while (lo < hi) { int m = (lo + hi) / 2; if (A[m] < t) lo = m + 1; else hi = m; }"
                " if (lo < %d && A[lo] == t) T[lo](c, ret); else c->fault = 4; }\n" % (len(funcs), len(funcs)))
    lib = os.path.join(tmp, "functions.so")
    subprocess.run(["cc", "-O1", "-shared", "-fPIC", "-w", "-o", lib, src], check=True)
    so = ctypes.CDLL(lib)

    class Cpu(ctypes.Structure):
        _fields_ = [("r", ctypes.c_uint32 * 16), ("n", ctypes.c_uint32), ("z", ctypes.c_uint32), ("c", ctypes.c_uint32),
                    ("v", ctypes.c_uint32), ("fault", ctypes.c_int), ("steps", ctypes.c_uint32)]

    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    # the game's memory under the test, the same in both: the dump's regions (the program as it ran, its data and bss
    # live, the modules loaded), or without --ram only the program as the cartridge holds it (for Unicorn to run)
    regions = []
    if a.ram:
        for name, addr in RAM:
            data = open(os.path.join(a.ram, name), "rb").read()
            regions.append((addr, data))
    else:
        uc.mem_map(a.base, (len(code) + 0xFFF) & ~0xFFF)
        uc.mem_write(a.base, code)
    cbufs = [ctypes.create_string_buffer(d, len(d)) for _, d in regions]
    for addr, d in regions:
        uc.mem_map(addr, (len(d) + 0xFFF) & ~0xFFF)
        uc.mem_write(addr, d)
    LMAX = 1 << 22
    la, lv = (ctypes.c_uint32 * LMAX)(), (ctypes.c_uint8 * LMAX)()
    n = 1 + len(regions)
    ptrs = (ctypes.c_void_p * n)(None, *[ctypes.addressof(b) for b in cbufs])
    so.setup(n, ptrs, (ctypes.c_uint32 * n)(MB, *[r[0] for r in regions]),
             (ctypes.c_uint32 * n)(MS, *[len(r[1]) for r in regions]), la, lv, LMAX)
    so.where.restype = ctypes.POINTER(ctypes.c_uint8)
    heap_pages = [addr + o for addr, d in regions if addr == 0x08000000 for o in range(0, len(d), 0x1000)
                  if any(d[o:o + 0x1000])]
    written = set()  # what Unicorn wrote in the game's memory this trial

    def on_write(uc_, access, address, size_, value, user):
        written.update(range(address, address + size_))
    for addr, d in regions:
        uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=addr, end=addr + len(d) - 1)

    def original_byte(x):
        for addr, d in regions:
            if addr <= x < addr + len(d):
                return d[x - addr]
    uc.mem_map(MB, MS)
    uc.mem_map(RET, 0x1000)  # the return address holds "b .": Unicorn stops there (until) even when it reaches it mid-block
    uc.mem_write(RET, bytes.fromhex("feffffea") * 1024)
    rng = random.Random(4)
    matched, mismatched, untested = 0, [], 0
    for idx, (s, _, _) in enumerate(funcs):
        valid, bad = 0, None
        for _ in range(a.trials):
            # each register: a pointer into the test memory, any value, a small number, or (--ram) an address in the
            # game's used heap pages, where its objects are
            choices = [lambda: MB + rng.randrange(0, MS // 2, 4), lambda: rng.getrandbits(32), lambda: rng.randrange(0, 64)]
            if heap_pages:
                choices.append(lambda: rng.choice(heap_pages) + rng.randrange(0, 0x1000, 4))
            regs = [rng.choice(choices)() for _ in range(13)]
            mem = bytearray(rng.randbytes(MS))
            for o in range(0, MS, 4):  # half the words point into the test memory, so pointer chains stay inside it
                if rng.random() < 0.5:
                    mem[o:o + 4] = (MB + rng.randrange(0, MS - 0x400, 4)).to_bytes(4, "little")
            mem = bytes(mem)
            flags = rng.getrandbits(4)
            cpu = Cpu()
            for k in range(13):
                cpu.r[k] = regs[k]
            cpu.r[13], cpu.r[14] = MB + MS - 0x100, RET  # the same stack and return address as Unicorn's
            cpu.n, cpu.z, cpu.c, cpu.v = (flags >> 3) & 1, (flags >> 2) & 1, (flags >> 1) & 1, flags & 1
            cmem = ctypes.create_string_buffer(mem, MS)
            so.scratch(cmem)
            so.run(idx, ctypes.byref(cpu))
            if cpu.fault:
                so.undo()
                continue
            valid += 1
            uc.reg_write(UC_ARM_REG_CPSR, 0x10 | (flags << 28))  # first: a mode change banks sp and lr
            for k in range(13):
                uc.reg_write(REGS[k], regs[k])
            uc.reg_write(UC_ARM_REG_SP, MB + MS - 0x100)
            uc.reg_write(UC_ARM_REG_LR, RET)
            uc.mem_write(MB, mem)
            written.clear()
            c_wrote = {la[k] for k in range(so.logged())}
            try:
                uc.emu_start(s, RET, count=200000)
            except UcError as e:
                bad = "unicorn: %s" % e
                so.undo()
                for x in written:
                    uc.mem_write(x, bytes([original_byte(x)]))
                break
            # the game's memory: every byte either side wrote, compared, then both put back as the dump holds it
            diff = [(x, bytes(uc.mem_read(x, 1))[0], so.where(x)[0], original_byte(x)) for x in sorted(c_wrote | written)
                    if so.where(x)[0] != bytes(uc.mem_read(x, 1))[0]]
            so.undo()
            for x in written:
                uc.mem_write(x, bytes([original_byte(x)]))
            if uc.reg_read(UC_ARM_REG_PC) != RET:
                # Unicorn's instruction count ran out before the return (a long loop: X's 002A54A0 on a random length
                # read ~20,000 elements, 4 November); C's own cap counts blocks and went further: no result to compare
                valid -= 1
                continue
            ur = [uc.reg_read(r) for r in REGS] + [uc.reg_read(UC_ARM_REG_SP)]
            cp = uc.reg_read(UC_ARM_REG_CPSR) >> 28
            cr = [cpu.r[k] for k in range(14)]
            cf = (cpu.n << 3) | (cpu.z << 2) | (cpu.c << 1) | cpu.v
            if ur != cr:
                bad = "registers: %s" % [(k, hex(ur[k]), hex(cr[k])) for k in range(14) if ur[k] != cr[k]][:3]
            elif cp != cf:
                bad = "flags: arm %X, c %X" % (cp, cf)
            elif bytes(uc.mem_read(MB, MS)) != cmem.raw:
                bad = "memory"
            elif diff:
                bad = "the game's memory at %s" % ", ".join("%08X (arm %02X, c %02X, before %02X)" % d for d in diff[:3])
            if bad:
                break
        if bad:
            mismatched.append((s, bad))
        elif valid == 0:
            untested += 1
        else:
            matched += 1
    print("%d functions in the list; %d translated (%d of them call others); %d matched, %d mismatched, %d with no valid "
          "trial" % (len(starts), len(funcs), with_calls, matched, len(mismatched), untested))
    for s, why in mismatched[:20]:
        print("  MISMATCH %08X %s" % (s, why))
    print("not translated, by first reason:")
    for k, v in sorted(reasons.items(), key=lambda kv: -kv[1])[:12]:
        print("  %6d  %s" % (v, k))


if __name__ == "__main__":
    main()
