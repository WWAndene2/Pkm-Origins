#!/usr/bin/env python3
"""The matching check of docs/RECOMPILATION_PLAN.md §4 for the translator (steps 3 and 5). Every function of the list that
tools/arm2c.py translates, and whose callees it translates too, is compiled natively and run against the original ARM
code executed by Unicorn (an ARM CPU emulator, the original instructions themselves) on the same random registers and
memory, the stack inside it, over the game's own memory from a running game with --ram (writes to it logged on both
sides, compared, then undone); a function matches when registers r0-r12 and sp, the flags, the VFP registers and FPSCR's flags and the whole test memory are
equal after every trial. Its calls run in both: the original callee in Unicorn, the translated one in C.

  match_functions.py <code.bin> <functions.tsv> [--trials 200] [--limit N] [--base 0x100000] [--keep DIR] [--jobs N]

Trials where the C stops on a fault (an access out of the 64 KiB test memory, or unaligned) are skipped, not counted as
matches; a function with no valid trial is reported apart. --keep DIR writes the generated C there (outside the
repository: it holds the game's literal constants). Needs capstone, unicorn and a C compiler (cc).
Unicorn stands in for Azahar here (same ARM instructions, no 3DS system); whole-game checks use Azahar (§4).
"""
import argparse
import bisect
import json
import mmap
import multiprocessing
import ctypes
import os
import random
import re
import struct
import sys
import time
import subprocess
import tempfile

from unicorn import UC_ARCH_ARM, UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc, UcError
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                               UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP,
                               UC_ARM_REG_PC, UC_ARM_REG_FPEXC, UC_ARM_REG_FPSCR, UC_ARM_REG_S0, UC_ARM_REG_C13_C0_3)

import arm2c

REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
        UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12]
# the FPSCR every 3DS thread runs with (Azahar's kernel/thread.cpp): default NaN, flush to zero, round toward zero
FPSCR = 0x03C00010
# the thread's local storage (mrc p15, 0, rX, c13, c0, 3): in the test memory, its words random as the rest; and an
# ARM clrex on the return page, run before each trial
TLS, CLREX = 0x1F008000, 0x1E000800
# the random test memory and the return address, away from the game's own memory (heap 0x08000000, stack below
# 0x10000000, linear heap 0x14000000)
MB, MS, RET = 0x1F000000, 0x10000, 0x1E000000
# --ram: the game's memory from a running game (Pomegrade's headless Azahar, "dump ADDRESS LENGTH FILE"), the files at
# these addresses
RAM = (("code.bin", 0x00100000), ("heap.bin", 0x08000000), ("linear.bin", 0x14000000), ("stack.bin", 0x0FF00000))

EDGES = [0, 0x80000000, 1, 0x807FFFFF, 0x00800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001, 0xFFC00001,
         0x7F7FFFFF, 0x4F000000, 0xCF000000, 0x4F800000, 0x3F800000, 0xBF000000]

PRELUDE = r"""
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <fenv.h>
typedef struct { uint32_t r[16]; uint32_t n, z, c, v; int fault; uint32_t steps; uint32_t s[32], fpscr; uint32_t tls, mon, mon_addr, depth; } Cpu;
/* memory regions: 0 is the random test memory, the others the game's memory (--ram), whose writes are logged (address,
   old byte) so that a trial can be compared on what it wrote and undone after */
extern uint8_t* RP[8]; extern uint32_t RB[8], RS[8]; extern int NR;
extern uint32_t* LA; extern uint8_t* LV; extern int NL, LMAX;
extern uint32_t RO_LO, RO_HI; /* the program's text and read-only data: read-only on the 3DS, a write faults (7) */
static uint8_t* at(Cpu* c, uint32_t a, uint32_t n, int w)
{
    if (a % n) { c->fault = 1; return 0; }
    if (w && a + n > RO_LO && a < RO_HI) { c->fault = 7; return 0; }
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
"""
# the trial machinery, in the first of the C files only (the functions are spread over several, compiled at once)
MAIN = r"""
uint8_t* RP[8]; uint32_t RB[8], RS[8]; int NR;
uint32_t* LA; uint8_t* LV; int NL, LMAX;
uint32_t RO_LO, RO_HI;
void protect(uint32_t lo, uint32_t hi) { RO_LO = lo; RO_HI = hi; }
void setup(int nr, uint8_t** p, uint32_t* b, uint32_t* s, uint32_t* la, uint8_t* lv, int lmax)
{ NR = nr; for (int i = 0; i < nr; i++) { RP[i] = p[i]; RB[i] = b[i]; RS[i] = s[i]; } LA = la; LV = lv; LMAX = lmax; }
void scratch(uint8_t* p) { RP[0] = p; NL = 0; }
int logged(void) { return NL; }
uint8_t* where(uint32_t a) { for (int i = 1; i < NR; i++) if (a - RB[i] < RS[i]) return RP[i] + (a - RB[i]); return 0; }
/* the system calls, as the trials stand them in: the same mixing of the call's number and r0-r7 written to r0-r3 on both
   sides (svc_stand_in in Python for Unicorn), so that what the function does around a call is compared; what the call
   itself does is src/hos/'s, checked apart */
void hos_svc(Cpu* c, uint32_t n)
{
    uint32_t h = n * 0x9E3779B1u;
    for (int k = 0; k < 8; k++) { h ^= c->r[k]; h *= 0x85EBCA6Bu; h ^= h >> 13; }
    for (int k = 0; k < 4; k++) { c->r[k] = h; h *= 0xC2B2AE35u; h ^= h >> 16; }
}
void undo(void) { while (NL) { NL--; *where(LA[NL]) = LV[NL]; } }
"""


TRANSLATE = None  # (code, base, sizes, ARM starts, relocated words), set before the translating processes fork


def svc_stand_in(n, r):
    """hos_svc of the trials' C, for Unicorn: r0-r3 from the call's number and r0-r7"""
    h = (n * 0x9E3779B1) & 0xFFFFFFFF
    for k in range(8):
        h ^= r[k]
        h = (h * 0x85EBCA6B) & 0xFFFFFFFF
        h ^= h >> 13
    out = []
    for _ in range(4):
        out.append(h)
        h = (h * 0xC2B2AE35) & 0xFFFFFFFF
        h ^= h >> 16
    return out


def translate_one(s, end=None, is_thumb=None):
    """(s, ((C, size), callees)), or (s, the reason it is not translated). end and is_thumb: an entry inside a function
    (a branch's target there), translated to that function's end in its instruction set"""
    code, base, size, arm, thumb, relocated = TRANSLATE
    try:
        callees = set()
        c = arm2c.translate(code, base, s, end or s + min(size[s] or 0x400, 0x4000), "f_%08X" % s, arm, callees,
                            relocated, s in thumb if is_thumb is None else is_thumb, thumb, set())
        return s, (c, callees)
    except arm2c.Unsupported as x:
        return s, "instruction " + str(x).split()[1] if str(x).startswith("instruction") else str(x)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("code")
    p.add_argument("functions")
    p.add_argument("--trials", type=int, default=200)
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--base", type=lambda s: int(s, 0), default=0x100000)
    p.add_argument("--keep")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="processes running the trials (default: one per CPU)")
    p.add_argument("--cro", help="the game's modules (static.crs): the words of the program they rewrite are read when run")
    p.add_argument("--uses", help="test only the functions with one of these instructions (a regular expression on the "
                   "mnemonic: 'ldrex|strex', 'v(add|mul)'), and what they call: a quick check of a new translation")
    p.add_argument("--only", type=lambda v: int(v, 16), help="test this function only (and what it calls)")
    p.add_argument("--exheader", help="the program's exheader: its text and read-only data are made read-only, as on "
                   "the 3DS (a trial writing there is impossible in the game: X's 00507F50 on a random index wrote its "
                   "own code, 4 November)")
    p.add_argument("--ram", help="a folder of the game's memory (code.bin, heap.bin, linear.bin, stack.bin) under the tests")
    a = p.parse_args()
    code = open(a.code, "rb").read()
    rows = [l.split() for l in open(a.functions) if l.strip() and not l.startswith("#")]
    starts = sorted(int(r[0], 16) for r in rows)
    thumb = {int(r[0], 16) for r in rows if r[3] == "thumb"}  # (arm2c translates both)
    size = {int(r[0], 16): int(r[4]) for r in rows}
    arm = frozenset(x for x in starts if x not in thumb)
    relocated = arm2c.static_relocations(a.cro) if a.cro else frozenset()
    funcs, reasons, deps = {}, {}, {}
    global TRANSLATE
    TRANSLATE = (code, a.base, size, arm, frozenset(thumb), relocated)
    with multiprocessing.get_context("fork").Pool(a.jobs) as pool_:  # translating is a third of a run: shared too
        done = pool_.map(translate_one, starts, chunksize=256)
    why = {}  # each refused function's own reason
    for s, got in done:
        if isinstance(got, str):
            reasons[got] = reasons.get(got, 0) + 1
            why[s] = got
        else:
            (c, n), callees = got
            funcs[s], deps[s] = (c, n), callees
    # the entries inside functions their branches reach (shared tails), translated as functions of their own, from
    # there to the end of the function holding them, until none is new (an entry's code may branch into another)
    entry_thumb = set()
    while True:
        new = {t for d in deps.values() for t in d if t not in funcs and t not in why and t not in size}
        if not new:
            break
        for t in sorted(new):
            k = bisect.bisect(starts, t) - 1
            f = starts[k] if k >= 0 else None
            if f is None or t >= f + (size[f] or 0):
                why[t] = "branch to no function's code"
                reasons[why[t]] = reasons.get(why[t], 0) + 1
                continue
            _, got = translate_one(t, f + min(size[f], 0x4000), f in thumb)
            if isinstance(got, str):
                why[t] = got
                reasons["(entry) " + got] = reasons.get("(entry) " + got, 0) + 1
            else:
                (c, n), callees = got
                funcs[t], deps[t] = (c, n), callees
                if f in thumb:
                    entry_thumb.add(t)
    thumb |= entry_thumb  # (an entry in a Thumb function is Thumb: its address in the table and Unicorn's start)
    # a function whose callee is not translated is not either, until none is left
    while True:
        drop = [s for s in funcs if not deps[s] <= funcs.keys()]
        if not drop:
            break
        for s in drop:
            del funcs[s]
            reasons["calls an untranslated function"] = reasons.get("calls an untranslated function", 0) + 1
    # what each refused function waits for: its own reason, or those of the refused functions under it (iterated to a
    # fixed point: calls can loop); a reason's weight is the functions that wait for it alone, translated once it is
    roots = {s: {why[s]} for s in why}
    waiting = [s for s in deps if s not in funcs]
    for s in waiting:
        roots[s] = set()
    changed = True
    while changed:
        changed = False
        for s in waiting:
            new = set().union(*(roots.get(t, set()) for t in deps[s] if t not in funcs)) | roots[s]
            if new != roots[s]:
                roots[s], changed = new, True
    alone = {}
    for r in roots.values():
        if len(r) == 1:
            k = next(iter(r))
            alone[k] = alone.get(k, 0) + 1
    if a.uses:  # the functions with such an instruction (the disassembly each C line carries in its comment)
        uses = re.compile(r"/\* [0-9A-F]{8} (%s)\b" % a.uses)
        chosen = sorted(f for f in funcs if uses.search(funcs[f][0]))
    if a.limit or a.only or a.uses:  # the first N (or the one asked, or --uses), and every function they call
        keep, todo = set(), [a.only] if a.only else chosen if a.uses else sorted(funcs)[:a.limit]
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
    # the C in one file per job (compiled at once), each with the declarations; the first has the trial machinery and
    # the tables
    protos = "".join("void f_%08X(Cpu* c, uint32_t ret);\n" % s for s, _, _ in funcs)
    parts = max(1, a.jobs)
    srcs = [os.path.join(tmp, "functions%d.c" % k) for k in range(parts)]
    for k, path in enumerate(srcs):
        with open(path, "w") as f:
            f.write(PRELUDE + arm2c.RUNTIME + protos)
            for s, c, _ in funcs[k::parts]:
                f.write(c + "\n")
    src = srcs[0]
    with open(src, "a") as f:
        f.write(MAIN)
        # call_indirect: a binary search of the translated functions by address (a Thumb one's with bit 0 set, as a
        # pointer to it holds it: an even address to Thumb code, or odd to ARM, finds nothing) (an untranslated target: fault 4,
        # the trial does not count)
        f.write("static const uint32_t A[] = {%s};\n" % ", ".join("0x%Xu" % (s | (s in thumb)) for s, _, _ in funcs))
        f.write("typedef void (*Fn)(Cpu*, uint32_t);\nstatic const Fn T[] = {%s};\n" % ", ".join("f_%08X" % s for s, _, _ in funcs))
        f.write("void run(int i, Cpu* c) { fesetround(FE_TOWARDZERO); T[i](c, c->r[14]); fesetround(FE_TONEAREST); }\n")
        f.write("void call_indirect(Cpu* c, uint32_t t, uint32_t ret) { int lo = 0, hi = %d;"
                " while (lo < hi) { int m = (lo + hi) / 2; if (A[m] < t) lo = m + 1; else hi = m; }"
                " if (lo < %d && A[lo] == t) T[lo](c, ret); else c->fault = 4; }\n" % (len(funcs), len(funcs)))
    lib = os.path.join(tmp, "functions.so")
    flags = ["-O1", "-fPIC", "-w", "-frounding-math", "-ffp-contract=off"]
    compiling = [subprocess.Popen(["cc", "-c"] + flags + ["-o", path + ".o", path]) for path in srcs]
    if any(p_.wait() for p_ in compiling):
        raise SystemExit("the generated C does not compile")
    subprocess.run(["cc", "-shared", "-o", lib] + [path + ".o" for path in srcs] + ["-lm"], check=True)
    so = ctypes.CDLL(lib)

    class Cpu(ctypes.Structure):
        _fields_ = [("r", ctypes.c_uint32 * 16), ("n", ctypes.c_uint32), ("z", ctypes.c_uint32), ("c", ctypes.c_uint32),
                    ("v", ctypes.c_uint32), ("fault", ctypes.c_int), ("steps", ctypes.c_uint32),
                    ("s", ctypes.c_uint32 * 32), ("fpscr", ctypes.c_uint32),
                        ("tls", ctypes.c_uint32), ("mon", ctypes.c_uint32), ("mon_addr", ctypes.c_uint32),
                    ("depth", ctypes.c_uint32)]

    # the random test memories: a pool made once (half the words point into the test memory, so pointer chains stay
    # inside it), each trial taking one; drawing 16,384 words per trial was most of a run's time
    prng = random.Random(4)
    pool = []
    for _ in range(64):
        mem = bytearray(prng.randbytes(MS))
        for o in range(0, MS, 4):
            if prng.random() < 0.5:
                mem[o:o + 4] = (MB + prng.randrange(0, MS - 0x400, 4)).to_bytes(4, "little")
        pool.append(bytes(mem))

    def check(job, jobs):
        """the trials of every jobs-th function from job: (matched, mismatched, no valid trial)"""
        began = time.time()
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
        if a.exheader:  # from the program's start to its data segment (exheader 0x30: the data segment's address)
            so.protect(a.base, struct.unpack_from("<I", open(a.exheader, "rb").read(), 0x30)[0])
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

        def on_svc(uc_, intno, user):  # pc is past the svc: its number is in the instruction there
            pc = uc_.reg_read(UC_ARM_REG_PC)
            if intno != 2:
                uc_.emu_stop()
                return
            if uc_.reg_read(UC_ARM_REG_CPSR) & 0x20:
                n = int.from_bytes(uc_.mem_read(pc - 2, 2), "little") & 0xFF
            else:
                n = int.from_bytes(uc_.mem_read(pc - 4, 4), "little") & 0xFFFFFF
            for reg, v in zip(REGS, svc_stand_in(n, [uc_.reg_read(REGS[k]) for k in range(8)])):
                uc_.reg_write(reg, v)
        uc.hook_add(UC_HOOK_INTR, on_svc)
        uc.mem_map(RET, 0x1000)  # the return address holds "b .": Unicorn stops there (until) even when it reaches it mid-block
        uc.mem_write(RET, bytes.fromhex("feffffea") * 1024)
        uc.mem_write(CLREX, bytes.fromhex("1ff07ff5"))
        matched, mismatched, untested, thumb_matched = 0, [], 0, 0
        for idx, (s, _, _) in enumerate(funcs):
            if idx % jobs != job:
                continue
            if job == 0 and idx % (500 * jobs) == 0:  # progress, on stderr (a whole game is minutes)
                sys.stderr.write("  %d / %d functions tested, %d s\n" % (idx, len(funcs), time.time() - began))
                sys.stderr.flush()
            rng = random.Random(s)
            struct.pack_into("<I", current, 4 * job, s)
            valid, bad = 0, None
            for _ in range(a.trials):
                # each register: a pointer into the test memory, any value, a small number, or (--ram) an address in the
                # game's used heap pages, where its objects are
                choices = [lambda: MB + rng.randrange(0, MS // 2, 4), lambda: rng.getrandbits(32), lambda: rng.randrange(0, 64)]
                if heap_pages:
                    choices.append(lambda: rng.choice(heap_pages) + rng.randrange(0, 0x1000, 4))
                regs = [rng.choice(choices)() for _ in range(13)]
                mem = rng.choice(pool)
                flags = rng.getrandbits(4)
                # the VFP registers: any bits, an ordinary float, a small integer (vcvt's input), or a value at a rule's
                # edge (zeros, denormals, infinities, NaNs, the integer limits)
                fchoices = [lambda: rng.getrandbits(32), lambda: struct.unpack("<I", struct.pack("<f", rng.uniform(-1e3, 1e3)))[0],
                            lambda: rng.randrange(0, 256), lambda: rng.choice(EDGES)]
                fregs = [rng.choice(fchoices)() for _ in range(32)]
                fflags = rng.getrandbits(4)
                cpu = Cpu()
                for k in range(13):
                    cpu.r[k] = regs[k]
                cpu.r[13], cpu.r[14] = MB + MS - 0x100, RET  # the same stack and return address as Unicorn's
                cpu.n, cpu.z, cpu.c, cpu.v = (flags >> 3) & 1, (flags >> 2) & 1, (flags >> 1) & 1, flags & 1
                for k in range(32):
                    cpu.s[k] = fregs[k]
                cpu.fpscr = FPSCR | fflags << 28
                cpu.tls = TLS
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
                uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)  # VFP on
                uc.reg_write(UC_ARM_REG_C13_C0_3, TLS)
                uc.emu_start(CLREX, CLREX + 4)  # the exclusive monitor closed, as the C's starts (a trial may leave it open)
                uc.reg_write(UC_ARM_REG_FPSCR, FPSCR | fflags << 28)
                for k in range(32):
                    uc.reg_write(UC_ARM_REG_S0 + k, fregs[k])
                uc.mem_write(MB, mem)
                written.clear()
                c_wrote = {la[k] for k in range(so.logged())}
                try:
                    uc.emu_start(s | (s in thumb), RET, count=200000)
                except UcError as e:
                    bad = "unicorn: %s at pc %08X (cpsr %08X)" % (e, uc.reg_read(UC_ARM_REG_PC), uc.reg_read(UC_ARM_REG_CPSR))
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
                us = [uc.reg_read(UC_ARM_REG_S0 + k) for k in range(32)]
                ufp, cfp = uc.reg_read(UC_ARM_REG_FPSCR) >> 28, cpu.fpscr >> 28  # (its sticky exception bits are not kept)
                if ur != cr:
                    bad = "registers: %s" % [(k, hex(ur[k]), hex(cr[k])) for k in range(14) if ur[k] != cr[k]][:3]
                elif cp != cf:
                    bad = "flags: arm %X, c %X" % (cp, cf)
                elif us != list(cpu.s):
                    bad = "VFP registers: %s" % [(k, hex(us[k]), hex(cpu.s[k])) for k in range(32) if us[k] != cpu.s[k]][:3]
                elif ufp != cfp:
                    bad = "FPSCR flags: arm %X, c %X" % (ufp, cfp)
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
                thumb_matched += s in thumb
        return matched, mismatched, untested, thumb_matched
    # the functions shared between --jobs processes (forked: each its own Unicorn and its own copy of the C's state);
    # each function's trials are seeded by its address, so the result does not depend on the number of jobs
    # each job writes the function it is testing here: a job that dies (the C crashing) is reported with it
    current = mmap.mmap(-1, 4 * a.jobs)
    readers = []
    for job in range(a.jobs):
        r, w = os.pipe()
        if os.fork() == 0:
            os.close(r)
            with os.fdopen(w, "w") as out:
                json.dump(check(job, a.jobs), out)
            os._exit(0)
        os.close(w)
        readers.append(r)
    matched, mismatched, untested, thumb_matched = 0, [], 0, 0
    for r in readers:
        with os.fdopen(r) as f:
            text = f.read()
        if not text:
            job = readers.index(r)
            raise SystemExit("job %d died testing %08X (the translated C crashed: run --only %X --jobs 1)"
                             % (job, struct.unpack_from("<I", current, 4 * job)[0], struct.unpack_from("<I", current, 4 * job)[0]))
        m, bad, u, tm = json.loads(text)
        matched, untested, thumb_matched = matched + m, untested + u, thumb_matched + tm
        mismatched += [tuple(x) for x in bad]
    os.wait()  # (the children have all written: reap them)
    for _ in readers[1:]:
        os.wait()
    mismatched.sort()
    print("%d functions in the list; %d translated (%d of them call others); %d matched, %d mismatched, %d with no valid "
          "trial" % (len(starts), len(funcs), with_calls, matched, len(mismatched), untested))
    print("  Thumb: %d in the list, %d translated, %d matched" % (len(thumb - entry_thumb), sum(1 for f in funcs if f[0] in thumb),
                                                               thumb_matched))
    for s, why in mismatched[:20]:
        print("  MISMATCH %08X %s" % (s, why))
    print("not translated, by first reason (and the functions that wait for that reason alone, it included):")
    for k, v in sorted(reasons.items(), key=lambda kv: -kv[1])[:12]:
        print("  %6d  %6s  %s" % (v, alone.get(k, "") if k != "calls an untranslated function" else "", k))
    print("the reasons that free the most functions alone:")
    for k, v in sorted(alone.items(), key=lambda kv: -kv[1])[:12]:
        print("  %6d  %s" % (v, k))


if __name__ == "__main__":
    main()
