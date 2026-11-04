#!/usr/bin/env python3
"""Our function lists checked against Ghidra's (step 4 of docs/RECOMPILATION_PLAN.md §8): every function only one side
has is put in a class that says which side is right, so that a disagreement is never left unexplained.

  ghidra_compare.py <code.bin> <exheader.bin> <cro dir> <functions.tsv> <modules.tsv> <our list> <our cro list>

functions.tsv is Ghidra's (Pomegrade's tools/remake/ghidra/Export.java: entry, size in bytes...), from the program
Pomegrade's session_setup.sh builds (the .code at 0x100000 and every module linked from 0x10000000 by
tools/remake/prototype/cro_link.py); modules.tsv is cro_link.py's map, which turns a linked address back into a module
and its offset. Ghidra is not the reference: it is a second, independent reading.

Ghidra only, by class:
  nop entry        our start is the nop just before Ghidra's, and a call, pointer or import reaches that nop: the
                   function opens there (Ghidra starts at the push after it)
  padding nop      Ghidra starts on a nop no reference reaches, ours on the instruction after it: padding
  branch target    inside one of our functions, reached by a branch of it (a test before the push, a loop head): one
                   function, which Ghidra cuts in two
  nop entry, ours by the flow pass   the same, with no reference to the nop (printed: read them)
  mid-code         inside one of our functions, reached by no branch of it: Ghidra cuts running code (printed)
  data             past the text segment (rodata): data Ghidra read as code
  other            none of these (printed)
Ours only, by the evidence for it: call (a bl / blx in walked code), pointer, import, export (each a reference), and
flow (after the end of the function before it) split by whether any word or branch names it.
"""
import argparse
import bisect
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cro_functions  # noqa: E402

NOP = 0xE1A00000


def branch_target(w, a):
    if (w & 0x0E000000) == 0x0A000000 and w >> 28 != 0xF:
        imm = w & 0xFFFFFF
        return a + 8 + ((imm - (1 << 24)) if imm & 0x800000 else imm) * 4
    return None


def main():
    p = argparse.ArgumentParser()
    for name in ("code", "exheader", "cro_dir", "ghidra", "modules", "ours", "ours_cro"):
        p.add_argument(name)
    a = p.parse_args()
    base = 0x100000
    code = open(a.code, "rb").read()
    text_end = base + struct.unpack_from("<I", open(a.exheader, "rb").read(), 0x18)[0]
    mods = {m.name: m.data for m in cro_functions.load(a.cro_dir)}
    bases = sorted((int(f[1], 16), int(f[2], 16), f[0]) for f in
                   (line.rstrip("\n").split("\t") for line in open(a.modules)) if len(f) == 3 and f[0])
    ghidra = set()
    for line in open(a.ghidra):
        addr = int(line.split("\t")[0], 16)
        if addr < 0x10000000:
            ghidra.add(("", addr))
        else:
            i = bisect.bisect([b[0] for b in bases], addr) - 1
            b, size, m = bases[i]
            ghidra.add((m, addr - b) if addr < b + size else ("?", addr))
    ours = {}
    for line in open(a.ours):
        if not line.startswith("#"):
            f = line.rstrip("\n").split("\t")
            ours[("", int(f[0], 16))] = f[1]
    for line in open(a.ours_cro):
        if not line.startswith("#"):
            f = line.rstrip("\n").split("\t")
            ours[(f[0], int(f[1], 16))] = f[2]
    by_place = collections.defaultdict(list)
    for k in ours:
        by_place[k[0]].append(k[1])
    for v in by_place.values():
        v.sort()

    def data_of(m):
        return (code, base) if m == "" else (mods[m], 0)

    for part, sel in ((".code", lambda k: k[0] == ""), ("modules", lambda k: k[0] != "")):
        g = {k for k in ghidra if sel(k)}
        o = {k for k in ours if sel(k)}
        print("%s: Ghidra %d, ours %d, both %d, Ghidra only %d, ours only %d"
              % (part, len(g), len(o), len(g & o), len(g - o), len(o - g)))
        classes, examples = collections.Counter(), collections.defaultdict(list)
        for m, addr in sorted(g - o):
            if m == "?" or m == "" and addr >= text_end:
                key = "data"
            else:
                d, b = data_of(m)
                word = lambda x: struct.unpack_from("<I", d, x - b)[0]
                starts = by_place[m]
                i = bisect.bisect(starts, addr) - 1
                prev = starts[i] if i >= 0 else None
                nxt = starts[i + 1] if i + 1 < len(starts) else None
                if prev is not None and addr - prev == 4 and word(prev) == NOP:
                    key = "nop entry" if ours[(m, prev)] != "flow" else "nop entry, ours by the flow pass"
                elif addr % 4 == 0 and word(addr) == NOP and nxt == addr + 4:
                    key = "padding nop"
                elif prev is not None and any(branch_target(word(x), x) == addr for x in range(prev, addr, 4)) or \
                        prev is not None and addr % 2 == 0 and thumb_branch_to(d, b, prev, addr):
                    key = "branch target"
                elif prev is not None:
                    key = "mid-code"
                else:
                    key = "other"
            classes[key] += 1
            examples[key].append("%s+%X" % (m, addr) if m else "%X" % addr)
        for key, n in classes.most_common():
            print("  Ghidra only, %s: %d (%s)" % (key, n, ", ".join(examples[key][:3])))
        evidence = collections.Counter(ours[k] for k in o - g)
        print("  ours only, by evidence: %s" % ", ".join("%s %d" % kv for kv in evidence.most_common()))
        if part == ".code":
            words = collections.Counter(struct.unpack_from("<I", code, x)[0] for x in range(0, len(code) - 3, 4))
            targets = {branch_target(struct.unpack_from("<I", code, x)[0], base + x) for x in range(0, text_end - base - 3, 4)}
            flow = [k[1] for k in o - g if ours[k] == "flow"]
            named = sum(1 for x in flow if words[x] or words[x | 1] or x in targets)
            print("  ours only by the flow pass: %d named by a word or a branch, %d by none" % (named, len(flow) - named))


def thumb_branch_to(d, b, start, addr):
    """whether a Thumb b / b<cond> between start and addr jumps to addr"""
    for x in range(start, addr, 2):
        h = struct.unpack_from("<H", d, x - b)[0]
        if h >> 12 == 0xD and (h >> 8) & 0xF < 0xE:
            off = h & 0xFF
            if x + 4 + ((off - 256) if off & 0x80 else off) * 2 == addr:
                return True
        elif h >> 11 == 0x1C:
            off = h & 0x7FF
            if x + 4 + ((off - 2048) if off & 0x400 else off) * 2 == addr:
                return True
    return False


if __name__ == "__main__":
    main()
