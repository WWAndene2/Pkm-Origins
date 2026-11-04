#!/usr/bin/env python3
"""The function list (step 3, docs/RECOMPILATION_PLAN.md §3): every address the code calls directly (bl / blx imm targets)
or stores in a literal word that points at the start of a function-like instruction (vtables, callbacks), from our own
sweep, so the recompilation does not depend on a Ghidra project.

  function_list.py <code.bin> <out.tsv> [--base 0x100000]

out.tsv (committed under symbols/: addresses only, no game data): address, how it was found (call / pointer), the number
of direct calls to it. Pointers are a lower-confidence guess: a word in a data table that happens to land on code.
Measured 4 November on Omega Ruby: 15,617 entries (11,871 called, 3,746 by pointer), 14,492 of them also in Ghidra's 47,750
functions: incomplete. Missed: functions reached only by a jump (b, tail calls), through a pointer to one that does not
open with push, or only from the CROs. Ghidra's count is the larger reference until this sweep follows those.
Needs capstone.
"""
import argparse
import collections
import struct

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs
from capstone.arm import ARM_OP_IMM


def main():
    p = argparse.ArgumentParser()
    p.add_argument("code")
    p.add_argument("out")
    p.add_argument("--base", type=lambda s: int(s, 0), default=0x100000)
    a = p.parse_args()
    data = open(a.code, "rb").read()
    end = a.base + len(data)
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    calls = collections.Counter()
    for off in range(0, len(data) - 3, 4):
        w = struct.unpack_from("<I", data, off)[0]
        if (w & 0x0F000000) == 0x0B000000 and (w >> 28) != 0xF or (w >> 25) == 0x7D:  # bl (any cond), blx imm
            for i in md.disasm(data[off:off + 4], a.base + off):
                if i.operands and i.operands[0].type == ARM_OP_IMM and a.base <= i.operands[0].imm < end:
                    calls[i.operands[0].imm] += 1
    pointers = set()
    for off in range(0, len(data) - 3, 4):
        w = struct.unpack_from("<I", data, off)[0]
        if a.base <= w < end and w % 4 == 0 and w not in calls:
            first = struct.unpack_from("<I", data, w - a.base)[0]
            if first >> 16 == 0xE92D:  # push {..., lr}: a function's usual first instruction
                pointers.add(w)
    with open(a.out, "w") as f:
        f.write("# address\tfound by\tdirect calls\n")
        for addr in sorted(set(calls) | pointers):
            f.write("%08X\t%s\t%d\n" % (addr, "call" if addr in calls else "pointer", calls[addr]))
    print("%d functions: %d called directly, %d only through a pointer" % (len(calls) + len(pointers), len(calls), len(pointers)))


if __name__ == "__main__":
    main()
