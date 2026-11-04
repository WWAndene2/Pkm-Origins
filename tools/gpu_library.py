#!/usr/bin/env python3
"""Where the game's graphics library is in its .code (step 4 of docs/RECOMPILATION_PLAN.md §3: the library is
reimplemented on Vulkan, so its functions must first be found). Two marks, read from the code, never from a run:

- writers: functions whose literal pools hold PICA200 command headers, the words the GPU's command lists are made of
  (header: register id in bits 0-15, byte mask in 16-19, extra parameter count in 20-27, bit 31 for consecutive
  registers; 3dbrew's GPU/Internal Registers). Such a word is only proof when its register is one the GPU has: the
  ones counted are the registers of the GPU's stages (rasterizer 0x040-0x07F, texturing 0x080-0x0FF, fragment
  combiners and output 0x0C0-0x13F, geometry pipeline and shaders 0x200-0x2FF). A pool word can still be data that looks
  like one: a function is listed when it holds 3 or more.
- layers: every function's callers (direct bl / blx), walked up from the writers, so each function gets its distance
  from the nearest writer (0: a writer). The graphics library is the writers and the few layers above; past them is the
  engine (scenes, models) and the game.

  gpu_library.py <code.bin> <functions.tsv> [--names function_names.tsv] [--depth N] [--out out.tsv]

functions.tsv: symbols/<game>_functions.tsv (address, found by, calls, isa, size). --names: Pomegrade's
tools/remake/ghidra/function_names.tsv (Omega Ruby's names, read where it is: ORAS_ENGINE.md §6), to show what an
address is already known as. The output (addresses, counts, register ids, names: no game bytes) may be committed.
"""
import argparse
import struct
from collections import defaultdict

BASE = 0x00100000
GPU_REGISTERS = [(0x040, 0x080), (0x080, 0x0C0), (0x0C0, 0x140), (0x200, 0x300)]


def pica_register(w):
    """the register a word names if it reads as a PICA200 command header, else None"""
    if w & 0x7000F000:  # bits 12-15 and 28-30: unused in a header
        return None
    reg, mask = w & 0xFFFF, (w >> 16) & 0xF
    if not mask:
        return None
    if not any(lo <= reg < hi for lo, hi in GPU_REGISTERS):
        return None
    return reg


def read_functions(path):
    out = []
    for line in open(path):
        if line.startswith("#"):
            continue
        f = line.rstrip("\n").split("\t")
        out.append((int(f[0], 16), f[3], int(f[4])))
    return sorted(out)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("code")
    p.add_argument("functions")
    p.add_argument("--names")
    p.add_argument("--depth", type=int, default=3)
    p.add_argument("--out")
    a = p.parse_args()
    data = open(a.code, "rb").read()
    end = BASE + len(data)
    functions = read_functions(a.functions)
    names = {}
    if a.names:
        for line in open(a.names):
            if not line.startswith("#"):
                f = line.rstrip("\n").split("\t")
                if len(f) >= 2:
                    names[int(f[0], 16)] = f[1]

    regs = defaultdict(set)
    callers = defaultdict(set)
    starts = {s for s, _, _ in functions}
    for s, isa, size in functions:
        for at in range(s, min(s + size, end) & ~3, 4):
            if at < s:
                continue
            w = struct.unpack_from("<I", data, at - BASE)[0]
            r = pica_register(w)
            if r is not None:
                regs[s].add(r)
            if isa == "arm" and (w & 0x0F000000) == 0x0B000000 and (w >> 28) != 0xF:  # bl
                off = (w & 0xFFFFFF) << 2
                t = at + 8 + (off - (1 << 26) if off & (1 << 25) else off)
                if t in starts:
                    callers[t].add(s)
            elif (w >> 25) == 0x7D:  # blx imm (to Thumb)
                off = ((w & 0xFFFFFF) << 2) | ((w >> 23) & 2)
                t = at + 8 + (off - (1 << 26) if off & (1 << 25) else off)
                if t in starts:
                    callers[t].add(s)

    writers = {s for s, r in regs.items() if len(r) >= 3}
    layer = {s: 0 for s in writers}
    frontier = set(writers)
    for d in range(1, a.depth + 1):
        nxt = set()
        for f in frontier:
            for c in callers[f]:
                if c not in layer:
                    layer[c] = d
                    nxt.add(c)
        frontier = nxt

    counts = defaultdict(int)
    for d in layer.values():
        counts[d] += 1
    named = sum(1 for f in layer if f in names)
    print("writers (3+ PICA200 registers in their pools): %d" % len(writers))
    for d in sorted(counts):
        print("  layer %d: %d functions" % (d, counts[d]))
    print("already named in Pomegrade: %d" % named)
    if a.out:
        with open(a.out, "w") as out:
            out.write("# address\tlayer\tcallers\tregisters\tname (Pomegrade)\n")
            for f in sorted(layer, key=lambda f: (layer[f], f)):
                r = " ".join("%03X" % x for x in sorted(regs.get(f, ())))
                out.write("%08X\t%d\t%d\t%s\t%s\n" % (f, layer[f], len(callers[f]), r, names.get(f, "")))


if __name__ == "__main__":
    main()
