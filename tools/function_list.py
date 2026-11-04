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

--exheader adds a flow pass (the "flow" entries): from every known start, the code is followed to its end (a return or
an unconditional jump with no branch of the function reaching further, its literal pool skipped: the words its pc-relative
loads read, and zeros); the first instruction after it, inside the text segment (exheader), starts the next function.
A flow start a conditional branch of the function before it reaches is merged back into that function.
Code reached only by fall-through from such an end is a function by construction: the text segment is a run of functions.

Needs capstone.
"""
import argparse
import bisect
import collections
import struct

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs
from capstone.arm import ARM_OP_IMM


def main():
    p = argparse.ArgumentParser()
    p.add_argument("code")
    p.add_argument("out")
    p.add_argument("--base", type=lambda s: int(s, 0), default=0x100000)
    p.add_argument("--exheader", help="the exheader beside the code: enables the flow pass")
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
    flow = set()
    if a.exheader:
        text_end = a.base + struct.unpack_from("<I", open(a.exheader, "rb").read(), 0x18)[0]
        flow = flow_pass(data, a.base, text_end, set(calls) | pointers)
    kinds = {}
    for addr in flow:
        kinds[addr] = "flow"
    for addr in pointers:
        kinds[addr] = "pointer"
    for addr in calls:
        kinds[addr] = "call"
    with open(a.out, "w") as f:
        f.write("# address\tfound by\tdirect calls\n")
        for addr in sorted(kinds):
            f.write("%08X\t%s\t%d\n" % (addr, kinds[addr], calls[addr]))
    print("%d functions: %d called directly, %d only through a pointer, %d by the flow pass"
          % (len(kinds), len(calls), len(pointers), len(set(kinds) - set(calls) - pointers)))


def is_return(w):
    """The end of a function: bx lr, mov pc lr, pop / ldm with pc, ldr pc from the stack (any of them unconditional)."""
    if w >> 28 != 0xE:
        return False
    return (w == 0xE12FFF1E or w == 0xE1A0F00E
            or (w & 0x0E108000) == 0x08108000  # ldm ... {..., pc}
            or (w & 0x0E50F000) == 0x0410F000 and ((w >> 16) & 15) == 13  # ldr pc, [sp], ...
            or (w & 0x0FFFFFF0) == 0x012FFF10)  # bx rN: a tail jump through a register


def pc_relative_data(data, base, text_end):
    """Every word a pc-relative load reads anywhere in the text segment: ldr / ldrb, ldrh / ldrd / ldrsb / ldrsh, vldr."""
    out = set()
    for a in range(base, text_end - 3, 4):
        w = struct.unpack_from("<I", data, a - base)[0]
        if w >> 28 == 0xF:
            continue
        t = n = None
        up = w & 0x00800000
        if (w & 0x0E5F0000) == 0x041F0000:  # ldr / ldrb [pc, #imm]
            t, n = w & 0xFFF, 1
        elif (w & 0x0E5F0090) == 0x005F0090 and (w & 0x60):  # ldrh / ldrsb / ldrsh / ldrd [pc, #imm]
            t, n = ((w >> 4) & 0xF0) | (w & 0xF), 2
        elif (w & 0x0E1F0E00) == 0x0C1F0A00 and (w & 0x01000000):  # vldr s / d [pc, #imm]
            t, n = (w & 0xFF) * 4, 2 if w & 0x100 else 1
        if t is not None:
            t = a + 8 + (t if up else -t)
            out.update(t + 4 * k for k in range(n))
    return out


def flow_pass(data, base, text_end, known):
    """Partitions the text segment into functions (see the module notes); returns every start found, known ones included."""
    word = lambda a: struct.unpack_from("<I", data, a - base)[0]
    starts = set(a for a in known if base <= a < text_end)
    pool_words = pc_relative_data(data, base, text_end)
    called = sorted(starts)  # the starts known before the pass bound the forward branches
    queue = list(called)
    seen = set()
    while queue:
        start = queue.pop()
        if start in seen:
            continue
        seen.add(start)
        i = bisect.bisect_right(called, start)
        limit = called[i] if i < len(called) else text_end
        pool = set()
        reach = start  # the furthest address a branch of this function reaches
        a = start
        while a < text_end:
            if a in pool:
                a += 4
                continue
            w = word(a)
            cond = w >> 28
            if (w & 0x0F000000) == 0x0A000000 and cond != 0xF:  # b
                imm = w & 0xFFFFFF
                t = a + 8 + ((imm - (1 << 24)) if imm & 0x800000 else imm) * 4
                if a < t < limit:  # forward, before the next known function: inside this one
                    reach = max(reach, t)
                if cond == 0xE and a >= reach:
                    break
            elif (w & 0x0F7F0000) == 0x051F0000 and cond != 0xF:  # ldr rX, [pc, #imm]: a literal pool word
                off = w & 0xFFF
                t = a + 8 + (off if w & 0x00800000 else -off)
                pool.add(t)
                if (w >> 12) & 15 == 15:  # ldr pc, [pc, #imm]: a jump through the pool
                    if a >= reach:
                        break
            elif is_return(w) and a >= reach:
                break
            a += 4
        a += 4
        # the pool, padding, address tables; a function never opens on a conditional instruction (what follows a pool and
        # does not is data: tables, floats)
        while a < text_end and (a in pool or a in pool_words or word(a) >> 28 != 0xE or base <= word(a) < text_end):
            a += 4
        if a < text_end and a not in starts:
            starts.add(a)
            queue.append(a)
        elif a < text_end:
            queue.append(a)
    # a start the flow pass found that a conditional branch of the function before it reaches is part of that function
    # (its walk ended early at a return the branch jumps over): merged, until none is left
    cond_branches = []
    for a in range(base, text_end - 3, 4):
        w = word(a)
        if (w & 0x0F000000) == 0x0A000000 and w >> 28 < 0xE:
            imm = w & 0xFFFFFF
            cond_branches.append((a, a + 8 + ((imm - (1 << 24)) if imm & 0x800000 else imm) * 4))
    while True:
        ordered = sorted(starts)
        merged = set()
        for a, t in cond_branches:
            if t > a and t in starts and t not in known:
                i = bisect.bisect_right(ordered, a)
                if i < len(ordered) and ordered[i] == t:
                    merged.add(t)
        if not merged:
            return starts
        starts -= merged


if __name__ == "__main__":
    main()
