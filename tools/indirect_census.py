#!/usr/bin/env python3
"""The hard part of step 2 (docs/RECOMPILATION_PLAN.md §3, §6), measured: how much of the code jumps to an address known
only at run time, which a static translation to C must resolve (a table of every possible target, or a fallback that
looks the target up at run time).

  indirect_census.py <code.bin> [--base 0x100000]

code.bin from tools/n3ds_extract.py (outside the repository). Swept linearly as ARM: literal pools decode as junk and are
counted only when they decode as one of the kinds below, so the counts are an upper bound (stated in the output).
Kinds: direct calls (bl/blx imm), indirect calls (blx rN), indirect jumps (bx rN other than lr, mov pc / add pc / ldr pc
not popping a return: switch tables and tail calls through pointers), returns (bx lr, pop/ldm with pc).
Needs capstone (pip install capstone).
"""
import argparse
import collections

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs
from capstone.arm import ARM_OP_REG, ARM_REG_LR, ARM_REG_PC, ARM_REG_SP


def main():
    p = argparse.ArgumentParser()
    p.add_argument("code")
    p.add_argument("--base", type=lambda s: int(s, 0), default=0x100000)
    a = p.parse_args()
    data = open(a.code, "rb").read()
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    count = collections.Counter()
    for off in range(0, len(data) - 3, 4):
        insns = list(md.disasm(data[off:off + 4], a.base + off))
        if not insns:
            count["undecodable words (data)"] += 1
            continue
        i = insns[0]
        m = i.mnemonic.split(".")[0]
        ops = i.operands
        if m in ("bl", "blx") and ops and ops[0].type != ARM_OP_REG:
            count["direct calls"] += 1
        elif m == "blx":
            count["indirect calls (blx rN)"] += 1
        elif m == "bx":
            count["returns (bx lr)" if ops[0].reg == ARM_REG_LR else "indirect jumps (bx rN)"] += 1
        elif m in ("pop", "ldm", "ldmia", "ldmfd") and any(o.type == ARM_OP_REG and o.reg == ARM_REG_PC for o in ops):
            count["returns (pop pc)" if m == "pop" or (ops[0].reg == ARM_REG_SP) else "indirect jumps (ldm pc)"] += 1
        elif m in ("ldr", "mov", "add") and ops and ops[0].type == ARM_OP_REG and ops[0].reg == ARM_REG_PC:
            if m == "ldr" and len(ops) > 1 and ops[1].mem.base == ARM_REG_SP:
                count["returns (pop pc)"] += 1
            else:
                count["indirect jumps (%s pc: tables, pointers)" % m] += 1
    print("%s: %d words (upper bounds: data decoding as these kinds is counted too)" % (a.code, len(data) // 4))
    for k, v in sorted(count.items(), key=lambda kv: -kv[1]):
        print("  %8d  %s" % (v, k))


if __name__ == "__main__":
    main()
