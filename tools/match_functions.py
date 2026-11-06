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
import collections
import json
import mmap
import ctypes
import os
import random
import re
import struct
import sys
import time
import subprocess
import tempfile

from unicorn import (UC_ARCH_ARM, UC_ERR_FETCH_PROT, UC_ERR_FETCH_UNMAPPED, UC_HOOK_INTR, UC_HOOK_MEM_WRITE, UC_MODE_ARM,
                     UC_PROT_READ, UC_PROT_WRITE, Uc, UcError)
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                               UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP,
                               UC_ARM_REG_PC, UC_ARM_REG_FPEXC, UC_ARM_REG_FPSCR, UC_ARM_REG_S0, UC_ARM_REG_C13_C0_3)

import arm2c
import translate_program

REGS = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
        UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12]
# the FPSCR every 3DS thread runs with (Azahar's kernel/thread.cpp): default NaN, flush to zero, round toward zero
FPSCR = 0x03C00010
FPSCR_GAME = 0x03000000  # what both games' start writes (vmsr at Omega Ruby's 001235AC): round to nearest
RAW_MODULES = 0x40000000  # --modules all: the modules' files from here
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
         0x7F7FFFFF, 0x4F000000, 0xCF000000, 0x4F800000, 0x3F800000, 0xBF000000,
         # next to the smallest normal number and to 1: where a product or quotient is tiny before rounding and normal
         # after (ARM flushes it), and the halfway cases of round to nearest
         0x00800001, 0x80800001, 0x00FFFFFF, 0x3F800001, 0x3F7FFFFF, 0x3F000001, 0x3EFFFFFF, 0x1FFFFFFF, 0x20000001,
         # (a double's high word next to its smallest normal, 2^-1022, and 1: as the high half of a d register)
         0x00100000, 0x00100001, 0x800FFFFF, 0x3FF00000, 0x3FEFFFFF, 0x20000000]

PRELUDE = r"""
#define HOS_MATCHING 1
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <fenv.h>
typedef struct { uint32_t r[16]; uint32_t n, z, c, v; int fault; uint32_t steps; uint32_t s[32], fpscr; uint32_t tls, mon, mon_addr, depth; uint32_t unwind; } Cpu;
/* memory regions: 0 is the random test memory, the others the game's memory (--ram), whose writes are logged (address,
   old byte) so that a trial can be compared on what it wrote and undone after */
extern uint8_t* RP[8]; extern uint32_t RB[8], RS[8]; extern int NR;
extern uint32_t* LA; extern uint8_t* LV; extern int NL, LMAX;
extern uint32_t RO_LO[16], RO_HI[16]; extern int NRO; extern uint32_t FAULT_AT; /* the address a fault 1 was on */ /* read-only on the 3DS (the program's text and read-only data, the modules'
   code): a write faults (7) */
static uint8_t* at(Cpu* c, uint32_t a, uint32_t n, int w)
{
    /* (an unaligned word or halfword access is the ARM11's, as the 3DS runs it with unaligned access on, and the
       runtime's (Cpu.h: memcpy); the instructions that need alignment, ldrd / strd / ldm / stm, check it themselves) */
    if (w)
        for (int k = 0; k < NRO; k++)
            if (a + n > RO_LO[k] && a < RO_HI[k]) { c->fault = 7; return 0; }
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
    FAULT_AT = a;
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
uint32_t RO_LO[16], RO_HI[16]; int NRO; uint32_t FAULT_AT;
uint32_t fault_at(void) { uint32_t x = FAULT_AT; FAULT_AT = 0; return x; }
void protect(uint32_t lo, uint32_t hi) { if (NRO < 16) { RO_LO[NRO] = lo; RO_HI[NRO] = hi; NRO++; } }
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
/* the regions code can run from (call_indirect: a call elsewhere, into data, returns at once, as Unicorn's side does) */
uint32_t EX_LO[256], EX_SZ[256]; int NEX;
void executable(int n, uint32_t* lo, uint32_t* sz) { NEX = n; for (int i = 0; i < n; i++) { EX_LO[i] = lo[i]; EX_SZ[i] = sz[i]; } }
int in_code(uint32_t a) { for (int i = 0; i < NEX; i++) if (a - EX_LO[i] < EX_SZ[i]) return 1; return 0; }
void undo(void) { while (NL) { NL--; *where(LA[NL]) = LV[NL]; } }
"""


def compile_all(command, srcs, jobs):
    """compiles each source to <source>.o, at most jobs at once; whether all compiled"""
    running, ok = [], True
    for path in srcs:
        if len(running) >= jobs:
            ok = running.pop(0).wait() == 0 and ok
        running.append(subprocess.Popen(command + ["-o", path + ".o", path]))
    for p_ in running:
        ok = p_.wait() == 0 and ok
    return ok


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


# the fault codes the translated C sets (src/hos/Cpu.h, tools/arm2c.py, the harness above)
FAULTS = {1: "memory: an access outside the test's memory, or misaligned", 2: "too long: the loop or call depth cap",
          3: "returned elsewhere than its caller", 4: "an indirect call to no translated function",
          5: "a jump table's index out of its table", 6: "the write log full", 7: "a write to read-only memory",
          8: "a call that does not return returned", 9: "a function the build could not translate was called", 10: "an unsupported instruction or code reached"}


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
    p.add_argument("--module", help="test this CRO module's functions (from --cro, the list being the modules' list "
                   "symbols/*_cro_functions.tsv) at --module-base, where the game loaded it (--map its memory)")
    p.add_argument("--module-base", type=lambda v: int(v, 16), default=0)
    p.add_argument("--modules", choices=["all"], help="test every module of --cro, each mapped as its file from "
                   "0x40000000 (where the 3DS maps nothing): its relocated words read as the file has them, the same on "
                   "both sides")
    p.add_argument("--linked", help="with --modules all: tools/cro_link.c's modules.tsv, each module tested where it "
                   "was linked (its modules.bin and data.bin given with --map)")
    p.add_argument("--with-code", help="with --module: the .code's function list; the .code (the code argument) is "
                   "translated and compiled beside the module, so the module's calls into it run")
    p.add_argument("--map", action="append", default=[], help="FILE@ADDRESS[:data]: more of the game's memory under the tests (:data: "
                   "not code, as the modules' data segments)")
    p.add_argument("--only", type=lambda v: int(v, 16), help="test this function only (and what it calls)")
    p.add_argument("--exheader", help="the program's exheader: its text and read-only data are made read-only, as on "
                   "the 3DS (a trial writing there is impossible in the game: X's 00507F50 on a random index wrote its "
                   "own code, 4 November)")
    p.add_argument("--untested", help="write the functions with no valid trial there: address, then why their trials "
                   "were rejected (the translated C's fault codes, src/hos/Cpu.h and arm2c.py, and how many trials each)")
    p.add_argument("--replay", help="with --only, or --retest of one function: a regs.txt that src/hos/GpuTrace.c's $HOS_CAPTURE wrote at the "
                   "function's entry in a real run (its folder holds that moment's memory, given as --ram): the first "
                   "trial runs on those registers, the rest stay random")
    p.add_argument("--retest", help="a --untested file of an earlier run: only its functions are tested (all are still "
                   "translated, for the calls), as a second pass over another --ram dump (another moment of the game)")
    p.add_argument("--ram", help="a folder of the game's memory (code.bin, heap.bin, linear.bin, stack.bin) under the tests")
    a = p.parse_args()
    code = open(a.code, "rb").read()
    rows = [l.split() for l in open(a.functions) if l.strip() and not l.startswith("#")]
    # what is translated and tested: the .code, or CRO modules (--module, --modules all), each a unit (its name, where
    # it is in the game's memory, its bytes, its function rows, its relocated words); every function is keyed by its
    # address in the game's memory (a module's offset + where it is), named by arm2c.fname
    units, raw = [], []  # raw: (address, bytes) of modules mapped as their files (--modules all)
    if a.module or a.modules:
        import cro_functions
        mods = {m.name: m for m in cro_functions.load(a.cro)}
        names = sorted(mods) if a.modules == "all" else [a.module]
        at = RAW_MODULES
        linked = {}
        if a.linked:  # tools/cro_link.c's map: where each module was linked (its image given with --map)
            for line in open(a.linked):
                f_ = line.split()
                linked[f_[0]] = int(f_[1], 16)
        for name in names:
            m = mods[name]
            mrows = [[r[1], r[2], "0", r[3], r[4]] for r in rows if r[0] == name]
            if a.module:
                vb_ = a.module_base
            elif a.linked:
                vb_ = linked[name]
            else:  # (each file as it is, one after the other: what the matching compares does not depend on where)
                vb_, at = at, (at + len(m.data) + 0xFFF) & ~0xFFF
                raw.append((vb_, m.data))
            units.append((name, vb_, m.data, mrows, {p: m.code_target.get(p) for p in m.relocated}, m))
    else:
        units.append((None, 0, code, rows, arm2c.static_relocations(a.cro) if a.cro else frozenset(), None))
    funcs, deps, why, reasons, thumb, name_of, starts = {}, {}, {}, {}, set(), {}, []
    for module, vb, data, urows, relocated, _ in units:
        ustarts = sorted(int(r[0], 16) for r in urows)
        uthumb = {int(r[0], 16) for r in urows if r[3] == "thumb"}  # (arm2c translates both)
        usize = {int(r[0], 16): int(r[4]) for r in urows}
        uf, ud, uw, ur, ue = translate_program.translate_program(
            data, 0 if module else a.base, ustarts, usize, uthumb, relocated, module, a.jobs)
        for k, v in uf.items():
            funcs[vb + k], name_of[vb + k] = v, arm2c.fname(k, module)
        for k, v in ud.items():
            deps[vb + k] = {vb + t for t in v}
        for k, v in uw.items():
            why[vb + k] = v
            name_of.setdefault(vb + k, arm2c.fname(k, module))
        for k, v in ur.items():
            reasons[k] = reasons.get(k, 0) + v
        thumb |= {vb + t for t in uthumb | ue}
        starts += [vb + x for x in ustarts]
    entry_thumb = {x for x in thumb if x not in set(starts)}
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
    retest = None
    replay = [int(x, 16) for x in open(a.replay).read().split()] if a.replay else None
    if a.retest:
        retest = {int(l.split("\t")[0], 16) for l in open(a.retest) if not l.startswith("#")}
    # the function replayed: --only's, or with --retest (everything translated, for its indirect calls) its one function
    replay_at = a.only if a.only else (min(retest) if retest and len(retest) == 1 else None)
    with_calls = sum(1 for s in funcs if deps[s])
    funcs = [(s, c, n) for s, (c, n) in sorted(funcs.items())]
    # --with-code: the .code translated too, compiled beside the module and found by call_indirect (the module's calls
    # into the program run in both), not tested itself
    extra = []  # (address with the Thumb bit, C name, C)
    if units[0][0] and a.with_code:
        crow = [l.split() for l in open(a.with_code) if l.strip() and not l.startswith("#")]
        cstarts = sorted(int(r[0], 16) for r in crow)
        cthumb = {int(r[0], 16) for r in crow if r[3] == "thumb"}
        csize = {int(r[0], 16): int(r[4]) for r in crow}
        cf, cdeps, _, _, cent = translate_program.translate_program(
            open(a.code, "rb").read(), 0x100000, cstarts, csize, cthumb, arm2c.static_relocations(a.cro), None, a.jobs)
        while True:  # (only those whose callees are all translated: the C must link)
            drop = [x for x in cf if not cdeps[x] <= cf.keys()]
            if not drop:
                break
            for x in drop:
                del cf[x]
        extra = [(x | (x in cthumb or x in cent), arm2c.fname(x), cf[x][0]) for x in sorted(cf)]
    tmp = a.keep or tempfile.mkdtemp()
    if not a.keep:  # the generated C and its objects (about 600 MB a game) go when the run ends, however it ends
        import atexit
        import shutil
        main_pid = os.getpid()
        atexit.register(lambda: os.getpid() == main_pid and shutil.rmtree(tmp, ignore_errors=True))
    os.makedirs(tmp, exist_ok=True)
    # the C in one file per job (compiled at once), each with the declarations; the first has the trial machinery and
    # the tables
    protos = "".join("void %s(Cpu* c, uint32_t ret);\n" % name_of[s] for s, _, _ in funcs)
    protos += "".join("void %s(Cpu* c, uint32_t ret);\n" % name for _, name, _ in extra)
    protos += "".join("extern uint32_t B_%s;\n" % u[0] for u in units if u[0])
    parts = max(1, a.jobs, (len(funcs) + len(extra)) // 1500)  # (files of ~1,500 functions: cc's memory)
    srcs = [os.path.join(tmp, "functions%d.c" % k) for k in range(parts)]
    for k, path in enumerate(srcs):
        with open(path, "w") as f:
            f.write(PRELUDE + arm2c.RUNTIME + protos)
            for s, c, _ in funcs[k::parts]:
                f.write(c + "\n")
            for _, _, c in extra[k::parts]:
                f.write(c + "\n")
    src = srcs[0]
    with open(src, "a") as f:
        f.write(MAIN)
        for u in units:
            if u[0]:
                f.write("uint32_t B_%s = 0x%Xu;\n" % (u[0], u[1]))
        # call_indirect: a binary search of the translated functions by address (a Thumb one's with bit 0 set, as a
        # pointer to it holds it: an even address to Thumb code, or odd to ARM, finds nothing) (an untranslated target: fault 4,
        # the trial does not count)
        table = sorted([(s | (s in thumb), name_of[s]) for s, _, _ in funcs] + [e[:2] for e in extra])
        f.write("static const uint32_t A[] = {%s};\n" % ", ".join("0x%Xu" % k for k, _ in table))
        f.write("typedef void (*Fn)(Cpu*, uint32_t);\nstatic const Fn T[] = {%s};\n" % ", ".join(n for _, n in table))
        f.write("static const Fn R[] = {%s};\n" % ", ".join(name_of[s] for s, _, _ in funcs))  # (the tested)
        f.write("void run(int i, Cpu* c) { hos_fpscr_mode(c->fpscr); R[i](c, c->r[14]); fesetround(FE_TONEAREST); }\n")
        f.write("void call_indirect(Cpu* c, uint32_t t, uint32_t ret) { int lo = 0, hi = %d;"
                " while (lo < hi) { int m = (lo + hi) / 2; if (A[m] < t) lo = m + 1; else hi = m; }"
                " if (lo < %d && A[lo] == t) T[lo](c, ret); else if (in_code(t & ~1u) || c->r[14] != ret) { c->fault = 4; c->r[15] = t; } }\n" % (len(table), len(table)))
    lib = os.path.join(tmp, "functions.so")
    flags = ["-O1", "-fPIC", "-w", "-frounding-math", "-ffp-contract=off"]
    if not compile_all(["cc", "-c"] + flags, srcs, a.jobs):
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
    # and pools for the trials made of pointers (every other one): 7 words in 8 point into the test memory, so that a
    # function walking objects (a pointer, a field that is a pointer, its field...) reaches its end inside it
    dense = []
    for _ in range(16):
        mem = bytearray(prng.randbytes(MS))
        for o in range(0, MS, 4):
            if prng.random() < 0.875:
                mem[o:o + 4] = (MB + prng.randrange(0, MS - 0x400, 4)).to_bytes(4, "little")
        dense.append(bytes(mem))

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
        data_at = {0x08000000, 0x14000000, 0x0FF00000}  # the heap, linear heap and stack dumps: data, never run
        for spec in a.map:  # more of the game's memory: FILE@ADDRESS[:data] (the modules' region)
            path, addr = spec.rsplit("@", 1)
            if addr.endswith(":data"):
                addr = addr[:-5]
                data_at.add(int(addr, 16))
            regions.append((int(addr, 16), open(path, "rb").read()))
        if raw:  # --modules all: the modules' files, one region
            blob = bytearray(raw[-1][0] + len(raw[-1][1]) - raw[0][0])
            for at_, d in raw:
                blob[at_ - raw[0][0]:at_ - raw[0][0] + len(d)] = d
            regions.append((raw[0][0], bytes(blob)))
        cbufs = [ctypes.create_string_buffer(d, len(d)) for _, d in regions]
        # the data regions are mapped without execute permission: a call into one (a function pointer read from random
        # data) is stood in for, on both sides, by a function that returns at once to lr (the restart below,
        # call_indirect in the C), when lr is where the C returns to (a call, or a jump with lr kept); a call into code
        # that is not a translated function, or a jump with lr changed, stays a fault (4), the trial not counted
        # code segments, where known: the program's text (exheader 0x10, size 0x18) and each module's code segment;
        # the rest of their regions (read-only data, data) is data
        segments = []
        if a.exheader:
            ex = open(a.exheader, "rb").read()
            segments.append((struct.unpack_from("<I", ex, 0x10)[0], struct.unpack_from("<I", ex, 0x18)[0]))
        for u in units:
            if u[0]:
                segments.append((u[1] + u[5].code_start, u[5].code_end - u[5].code_start))
        executable = []
        for addr, d in regions:
            size = (len(d) + 0xFFF) & ~0xFFF
            inside = [(lo, n) for lo, n in segments if addr <= lo < addr + size]
            if addr in data_at or inside:
                uc.mem_map(addr, size, UC_PROT_READ | UC_PROT_WRITE)
                for lo, n in inside:  # (pages: a segment's edges rounded out)
                    p0, p1 = lo & ~0xFFF, (lo + n + 0xFFF) & ~0xFFF
                    uc.mem_protect(p0, min(p1, addr + size) - p0)
                    executable.append((p0, min(p1, addr + size) - p0))
            else:
                uc.mem_map(addr, size)
                executable.append((addr, size))
            uc.mem_write(addr, d)
        if not a.ram:
            executable.append((a.base, (len(code) + 0xFFF) & ~0xFFF))
        if len(executable) > 256:
            raise SystemExit("more than 8 code regions: raise the C's EX_LO size")
        so.executable(len(executable), (ctypes.c_uint32 * 256)(*[e[0] for e in executable]),
                      (ctypes.c_uint32 * 256)(*[e[1] for e in executable]))
        LMAX = 1 << 22
        la, lv = (ctypes.c_uint32 * LMAX)(), (ctypes.c_uint8 * LMAX)()
        n = 1 + len(regions)
        ptrs = (ctypes.c_void_p * n)(None, *[ctypes.addressof(b) for b in cbufs])
        so.setup(n, ptrs, (ctypes.c_uint32 * n)(MB, *[r[0] for r in regions]),
                 (ctypes.c_uint32 * n)(MS, *[len(r[1]) for r in regions]), la, lv, LMAX)
        so.where.restype = ctypes.POINTER(ctypes.c_uint8)
        if a.exheader:  # from the program's start to its data segment (exheader 0x30: the data segment's address)
            so.protect(a.base, struct.unpack_from("<I", open(a.exheader, "rb").read(), 0x30)[0])
        for u in units:  # each module's code segment, mapped read-only by ldr:ro
            if u[0]:
                so.protect(u[1] + u[5].code_start, u[1] + u[5].code_end)
        heap_pages = [addr + o for addr, d in regions if addr == 0x08000000 for o in range(0, len(d), 0x1000)
                      if any(d[o:o + 0x1000])]
        # and the heap's objects as the game refers to them: every aligned heap address a heap word points to (an
        # object's start, or a field the game keeps a pointer to), so that a register handed one finds real fields
        referenced = []
        for addr, d in regions:
            if addr == 0x08000000:
                words = struct.unpack("<%dI" % (len(d) // 4), d)
                referenced = sorted({w for w in words if addr <= w < addr + len(d) - 0x100 and not w & 3})

        def at_mapped(x):
            return MB <= x < MB + MS or any(addr <= x < addr + len(d) for addr, d in regions)
        so.fault_at.restype = ctypes.c_uint32
        listed = set(starts)  # every function of the list (a --limit run translates some of them only)
        written = set()  # what Unicorn wrote in the game's memory this trial

        def on_write(uc_, access, address, size_, value, user):
            written.update(range(address, address + size_))
        for addr, d in regions:
            uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=addr, end=addr + len(d) - 1)

        def original_byte(x):
            for addr, d in regions:
                if addr <= x < addr + len(d):
                    return d[x - addr]
        uc.mem_map(MB, MS, UC_PROT_READ | UC_PROT_WRITE)

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
        matched, mismatched, untested, thumb_matched = 0, [], [], 0
        for idx, (s, _, _) in enumerate(funcs):
            if idx % jobs != job or retest is not None and s not in retest:
                continue
            if job == 0 and idx % (500 * jobs) == 0:  # progress, on stderr (a whole game is minutes)
                sys.stderr.write("  %d / %d functions tested, %d s\n" % (idx, len(funcs), time.time() - began))
                sys.stderr.flush()
            rng = random.Random(s)
            struct.pack_into("<I", current, 4 * job, s)
            valid, bad = 0, None
            rejected = collections.Counter()  # why the trials did not count
            for trial in range(a.trials):
                # each register: a pointer into the test memory, any value, a small number, or (--ram) an address in the
                # game's used heap pages, where its objects are
                choices = [lambda: MB + rng.randrange(0, MS // 2, 4), lambda: rng.getrandbits(32), lambda: rng.randrange(0, 64)]
                if heap_pages:
                    choices.append(lambda: rng.choice(heap_pages) + rng.randrange(0, 0x1000, 4))
                if referenced:
                    choices.append(lambda: rng.choice(referenced))
                if trial % 2:  # a trial of pointers: every register one, into the test memory or the game's heap
                    regs = [rng.choice(choices[:1] + choices[3:])() for _ in range(13)]
                    mem = rng.choice(dense)
                else:
                    regs = [rng.choice(choices)() for _ in range(13)]
                    mem = rng.choice(pool)
                flags = rng.getrandbits(4)
                # the VFP registers: any bits, an ordinary float, a small integer (vcvt's input), or a value at a rule's
                # edge (zeros, denormals, infinities, NaNs, the integer limits)
                fchoices = [lambda: rng.getrandbits(32), lambda: struct.unpack("<I", struct.pack("<f", rng.uniform(-1e3, 1e3)))[0],
                            lambda: rng.randrange(0, 256), lambda: rng.choice(EDGES)]
                fregs = [rng.choice(fchoices)() for _ in range(32)]
                fflags = rng.getrandbits(4)
                # the rounding mode: the kernel's (toward zero) or the one the game sets at its start (to nearest)
                fpscr = rng.choice((FPSCR, FPSCR_GAME))
                sp = MB + MS - 0x100
                if replay and s == replay_at and trial == 0:  # the inputs the game gave it (sp on the game's own stack)
                    regs, sp = replay[:13], replay[13]
                    flags = (replay[16] << 3) | (replay[17] << 2) | (replay[18] << 1) | replay[19]
                    fregs, fpscr, fflags = replay[20:52], replay[52] & 0x0FFFFFFF, replay[52] >> 28
                cpu = Cpu()
                for k in range(13):
                    cpu.r[k] = regs[k]
                cpu.r[13], cpu.r[14] = sp, RET  # the same stack and return address as Unicorn's
                cpu.n, cpu.z, cpu.c, cpu.v = (flags >> 3) & 1, (flags >> 2) & 1, (flags >> 1) & 1, flags & 1
                for k in range(32):
                    cpu.s[k] = fregs[k]
                cpu.fpscr = fpscr | fflags << 28
                cpu.tls = TLS
                cmem = ctypes.create_string_buffer(mem, MS)
                so.scratch(cmem)
                so.run(idx, ctypes.byref(cpu))
                if cpu.fault:
                    why = FAULTS.get(cpu.fault, "fault %d" % cpu.fault)
                    if cpu.fault == 1:  # what the address was: misaligned, near 0 (a null pointer's field), or elsewhere
                        x = so.fault_at()
                        why = "memory: %s" % ("an instruction needing alignment (ldrd, strd, ldm, stm)" if x == 0 else
                                              "near 0 (a null pointer's field)" if x < 0x10000 else
                                              "across a region's end" if at_mapped(x) else
                                              "outside the game's and test's memory, at %02Xxxxxxx" % (x >> 24))
                    if cpu.fault == 4:  # where to: code that is not a translated function, or data (lr changed)
                        t = cpu.r[15] & ~1
                        why += " (into %s)" % ("a listed function this run does not test" if t in listed else
                                               "code, not a function's start" if any(lo <= t < lo + n for lo, n in executable)
                                               else "data")
                    rejected[why] += 1
                    so.undo()
                    continue
                valid += 1
                uc.reg_write(UC_ARM_REG_CPSR, 0x10 | (flags << 28))  # first: a mode change banks sp and lr
                for k in range(13):
                    uc.reg_write(REGS[k], regs[k])
                uc.reg_write(UC_ARM_REG_SP, sp)
                uc.reg_write(UC_ARM_REG_LR, RET)
                uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)  # VFP on
                uc.reg_write(UC_ARM_REG_C13_C0_3, TLS)
                uc.emu_start(CLREX, CLREX + 4)  # the exclusive monitor closed, as the C's starts (a trial may leave it open)
                uc.reg_write(UC_ARM_REG_FPSCR, fpscr | fflags << 28)
                for k in range(32):
                    uc.reg_write(UC_ARM_REG_S0 + k, fregs[k])
                uc.mem_write(MB, mem)
                written.clear()
                c_wrote = {la[k] for k in range(so.logged())}
                try:
                    start = s | (s in thumb)
                    for _ in range(1000):  # each call into data returns at once: Unicorn restarted at the return address
                        try:
                            uc.emu_start(start, RET, count=200000)
                            break
                        except UcError as e:
                            if e.errno not in (UC_ERR_FETCH_PROT, UC_ERR_FETCH_UNMAPPED) or \
                                    any(lo <= uc.reg_read(UC_ARM_REG_PC) < lo + n for lo, n in executable):
                                raise
                            lr = uc.reg_read(UC_ARM_REG_LR)
                            cpsr = uc.reg_read(UC_ARM_REG_CPSR)
                            uc.reg_write(UC_ARM_REG_CPSR, (cpsr & ~0x20) | (0x20 if lr & 1 else 0))
                            start = lr
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
                    rejected["the original ran past Unicorn's instruction count"] += 1
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
                untested.append((s, dict(rejected)))
            if replay and s == replay_at:
                sys.stderr.write("replay of %08X: %s\n" % (s, "mismatch: " + bad if bad else "no valid trial" if valid == 0
                                                             else "matched (%d valid trials)" % valid))
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
    matched, mismatched, untested, thumb_matched = 0, [], [], 0
    for r in readers:
        with os.fdopen(r) as f:
            text = f.read()
        if not text:
            job = readers.index(r)
            at = struct.unpack_from("<I", current, 4 * job)[0]
            raise SystemExit("job %d died %s" % (job, "testing %08X (the translated C crashed, or its error is above: "
                                                 "run --only %X --jobs 1)" % (at, at) if at else
                                                 "before its first function (its error is above)"))
        m, bad, u, tm = json.loads(text)
        matched, untested, thumb_matched = matched + m, untested + [tuple(x) for x in u], thumb_matched + tm
        mismatched += [tuple(x) for x in bad]
    os.wait()  # (the children have all written: reap them)
    for _ in readers[1:]:
        os.wait()
    mismatched.sort()
    print("%d functions in the list; %d translated (%d of them call others); %d matched, %d mismatched, %d with no valid "
          "trial%s" % (len(starts), len(funcs), with_calls, matched, len(mismatched), len(untested),
                                     " (of the %d retested)" % len(retest) if retest is not None else ""))
    print("  Thumb: %d in the list, %d translated, %d matched" % (len(thumb - entry_thumb), sum(1 for f in funcs if f[0] in thumb),
                                                               thumb_matched))
    for s, why in mismatched[:20]:
        print("  MISMATCH %08X %s" % (s, why))
    if untested:
        # each untested function by its main reason: the one most of its trials were rejected for
        main_reason = collections.Counter(max(why, key=why.get) if why else "no trial" for _, why in untested)
        print("no valid trial, by the reason most of a function's trials were rejected for:")
        for k, v in main_reason.most_common():
            print("  %6d  %s" % (v, k))
        if a.untested:
            with open(a.untested, "w") as out:
                out.write("# function\twhy its trials were rejected (trials)\n")
                for f, why in sorted(untested):
                    out.write("%08X\t%s\n" % (f, "; ".join("%s (%d)" % kv for kv in sorted(why.items(), key=lambda kv: -kv[1]))))
    print("not translated, by first reason (and the functions that wait for that reason alone, it included):")
    for k, v in sorted(reasons.items(), key=lambda kv: -kv[1])[:12]:
        print("  %6d  %6s  %s" % (v, alone.get(k, "") if k != "calls an untranslated function" else "", k))
    print("the reasons that free the most functions alone:")
    for k, v in sorted(alone.items(), key=lambda kv: -kv[1])[:12]:
        print("  %6d  %s" % (v, k))


if __name__ == "__main__":
    main()
