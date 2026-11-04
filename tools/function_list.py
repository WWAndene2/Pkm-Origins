#!/usr/bin/env python3
"""The function list (step 3, docs/RECOMPILATION_PLAN.md §3): every function of the .code, from our own reading of the
code, so the recompilation does not depend on a Ghidra project.

  function_list.py <code.bin> <out.tsv> [--exheader <exheader.bin>] [--cro <cro dir>] [--base 0x100000]

out.tsv (committed under symbols/: addresses only, no game data): address, how it was found, the number of direct calls
to it, its instruction set (arm / thumb), its size in bytes (to the end of its last instruction as the flow pass walked
it, at most to the next entry: its trailing literal pool and any data after it left out).

How a start is found, the first that applies giving its "found by":
  call     the target of a bl / blx in code the flow pass walked (a bl counted in data, or in Thumb code read as ARM, made
           false starts: the list grows from sure starts, round after round, until the calls are the same twice)
  pointer  a word anywhere in the program naming a push {..., lr} or the nop some functions open on (vtables, callbacks,
           the Pawn natives table), or, with bit 0 set, a Thumb push that reads as Thumb
  import   (--cro) a .code function the modules import: static.crs's exports, the modules' |static| imports
  flow     (--exheader) found by the flow pass below

The flow pass: from every known start the code is followed to its end, a return or an unconditional jump no branch of
the function reaches past; its literal pools (every word a pc-relative ldr / ldrh / ldrd / vldr reads) are skipped, and
a switch's table is read (ldr pc, [pc, rN]: its addresses; add pc, pc, rN: its b's), its cases being part of the
function. After the end, past pools, padding (zeros, nops, mov r8, r8) and address tables, the next function starts, if
it reads as code: an ARM one whose first instructions up to a return decode with no coprocessor other than VFP or cp15
(a data table whose first word reads as an instruction fails within a few words), or a Thumb one that reads as valid
ARMv6K Thumb along its branches up to a return (strictly, for one that opens on no push: text and switch tables read as
Thumb too). Details the code states where they apply: a b right after a pop of lr, or an unconditional b to a push, is a
tail call; a pop of lr or nop padding followed by a push ends a function that falls into the next one; a computed jump
(add / mov pc) ends one unless lr was just set to the next instruction (a call); a function ends at the latest where
another known one starts; a start a conditional branch of the function before it reaches is merged back into it.

Checked (4 November) against Ghidra's own analysis with tools/ghidra_compare.py, and every function decoded whole: see
docs/RECOMPILATION_PLAN.md §9.
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
    p.add_argument("--cro", help="the folder of the game's modules: adds the .code functions they import")
    a = p.parse_args()
    data = open(a.code, "rb").read()
    end = a.base + len(data)
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    text_end = end
    if a.exheader:
        text_end = a.base + struct.unpack_from("<I", open(a.exheader, "rb").read(), 0x18)[0]
    pool_words = pc_relative_data(data, a.base, text_end)
    # every bl / blx in the text segment, outside literal pools (data read as code gave 241 false starts in Omega Ruby):
    # target -> [(caller, blx)]
    callers = collections.defaultdict(list)
    for off in range(0, text_end - a.base - 3, 4):
        if a.base + off in pool_words:
            continue
        w = struct.unpack_from("<I", data, off)[0]
        if (w & 0x0F000000) == 0x0B000000 and (w >> 28) != 0xF or (w >> 25) == 0x7D:  # bl (any cond), blx imm
            for i in md.disasm(data[off:off + 4], a.base + off):
                if i.operands and i.operands[0].type == ARM_OP_IMM and a.base <= i.operands[0].imm < text_end:
                    callers[i.operands[0].imm].append((a.base + off, w >> 25 == 0x7D))
    imported, imported_thumb = cro_imports(a.cro, a.base, text_end) if a.cro else (set(), set())
    pointers, pointer_thumb = set(), set()
    half = lambda x: struct.unpack_from("<H", data, x - a.base)[0]
    for off in range(0, len(data) - 3, 4):
        w = struct.unpack_from("<I", data, off)[0]
        if a.base <= w < text_end and w % 4 == 0:
            first = struct.unpack_from("<I", data, w - a.base)[0]
            if first >> 16 == 0xE92D or first == 0xE1A00000:  # push {..., lr}: a function's usual start; or the nop
                pointers.add(w)  # some open on (Omega Ruby's 00370774, a Pawn native, from the natives table)
        elif a.base < w < text_end and w & 3 == 1:  # a Thumb pointer (address | 1) to a Thumb push {..., lr}
            if half(w - 1) >> 8 == 0xB5 and thumb_valid(half, w - 1, text_end):  # that reads as Thumb (Omega Ruby's
                pointers.add(w - 1)  # 0020FD3C is an ARM bl, 0xEB00xxxx, whose low half reads as a push)
                pointer_thumb.add(w - 1)
    # The list grows from sure starts (the entry point, pointers, imports): each round walks them with the calls of the
    # round before, then takes the targets of the bl / blx found in the code walked, until those are the same twice. A
    # "bl" counted anywhere (data placed in the text, Thumb code read as ARM) made false starts: 16 in Omega Ruby.
    calls = collections.Counter()
    blx_targets = set()
    flow, thumb = set(), set()
    rounds = 0
    while True:
        cover, ends = set(), {}
        seeds = {a.base} | pointers | imported | set(calls)
        if a.exheader:
            flow, thumb = flow_pass(data, a.base, text_end, seeds, pointer_thumb | imported_thumb | blx_targets, cover,
                                    ends)
        else:
            cover = None
        new = collections.Counter()
        for t, cs in callers.items():
            for c, blx in cs:
                if cover is None or c in cover:
                    new[t] += 1
                    if blx:
                        blx_targets.add(t)
        rounds += 1
        if set(new) == set(calls) or rounds == 12:
            calls = new
            break
        calls = new
    if rounds == 12:
        raise SystemExit("the calls did not settle in 12 rounds")
    pointers -= set(calls)
    kinds = {}
    for addr in flow:
        kinds[addr] = "flow"
    for addr in imported:
        kinds[addr] = "import"
    for addr in pointers:
        kinds[addr] = "pointer"
    for addr in calls:
        kinds[addr] = "call"
    with open(a.out, "w") as f:
        f.write("# address\tfound by\tdirect calls\tisa\tsize\n")
        ordered = sorted(kinds)
        for i, addr in enumerate(ordered):
            nxt = ordered[i + 1] if i + 1 < len(ordered) else text_end
            size = min(ends.get(addr, nxt), nxt) - addr
            f.write("%08X\t%s\t%d\t%s\t%d\n" % (addr, kinds[addr], calls[addr], "thumb" if addr in thumb else "arm", size))
    print("%d functions: %d called directly, %d only through a pointer, %d only imported by a module, %d by the flow pass"
          % (len(kinds), len(calls), len(pointers - set(calls)), len(imported - set(calls) - pointers),
             len(set(kinds) - set(calls) - pointers - imported)))
    print("%d of them Thumb" % len(thumb & set(kinds)))


def cro_imports(cro_dir, base, end):
    """the .code functions the modules import, and the Thumb ones among them: static.crs's named exports and every
    |static| import of the modules that lie in the text segment (base to end)"""
    import os
    import cro_functions
    static = cro_functions.Module(os.path.join(cro_dir, "static.crs"))
    out = set(static.named_exports().values())
    for m in cro_functions.load(cro_dir):
        for imp in m.imports().values():
            if imp[0] != "named" and imp[1] == "|static|":
                out.add(static.tag(imp[2]) if imp[0] == "anon" else static.indexed_export(imp[2]))
    # in the text segment only (the rest is data the modules import: vtables, globals); an odd address is a Thumb
    # pointer (address | 1) to the function just below
    out = {t for t in out if t is not None and base <= t < end}
    return {t & ~1 for t in out}, {t & ~1 for t in out if t & 1}


def is_return(w):
    """The end of a function: bx lr, mov pc lr, pop / ldm with pc, ldr pc from the stack or memory, bx rN, a computed
    add / sub / mov to pc (any of them unconditional; a jump table relative to pc is not an end)."""
    if w >> 28 != 0xE:
        return False
    rn = (w >> 16) & 15
    return (w == 0xE12FFF1E or w == 0xE1A0F00E
            or (w & 0x0E108000) == 0x08108000  # ldm ... {..., pc}
            or (w & 0x0C50F000) == 0x0410F000 and rn != 15  # ldr pc, [rN, ...]: from the stack, or a jump through memory
            or (w & 0x0FFFFFF0) == 0x012FFF10  # bx rN: a tail jump through a register
            # add / sub / mov pc, ...: a computed jump (Omega Ruby's 001AFAE0, add pc, r5, r6); not add pc, pc, rN,
            # whose table of b's follows
            or (w & 0x0C00F000) == 0x0000F000 and (w >> 21) & 15 in (2, 4, 13) and not (rn == 15 and (w >> 21) & 15 != 13))


def is_call_through(word, start, a):
    """whether the jump at a is a call: lr was just set to the next instruction (mov lr, pc right before it, or an
    add lr, pc, #k pointing past it a few instructions earlier: Omega Ruby's 001AE4F4 ... add pc, r8, r3)"""
    if a - 4 >= start and word(a - 4) == 0xE1A0E00F:
        return True
    for b in range(a - 4, max(start, a - 32) - 4, -4):
        w = word(b)
        if (w & 0xFFFFF000) == 0xE28FE000:  # add lr, pc, #imm (rotated immediate)
            rot = ((w >> 8) & 15) * 2
            imm = ((w & 0xFF) >> rot | (w & 0xFF) << (32 - rot)) & 0xFFFFFFFF if rot else w & 0xFF
            return b + 8 + imm == a + 4
    return False


def pc_relative_words(w, a):
    """the words the instruction w at a reads relative to pc: ldr / ldrb, ldrh / ldrsb / ldrsh / ldrd, vldr s / d
    (an empty tuple for any other instruction)"""
    if w >> 28 == 0xF:
        return ()
    t = n = None
    up = w & 0x00800000
    if (w & 0x0E5F0000) == 0x041F0000:  # ldr / ldrb [pc, #imm]
        t, n = w & 0xFFF, 1
    elif (w & 0x0E5F0090) == 0x005F0090 and (w & 0x60) and w & 0x00400000:  # ldrh / ldrsb / ldrsh / ldrd [pc, #imm]
        t, n = ((w >> 4) & 0xF0) | (w & 0xF), 2
    elif (w & 0x0E1F0E00) == 0x0C1F0A00 and (w & 0x01000000):  # vldr s / d [pc, #imm]
        t, n = (w & 0xFF) * 4, 2 if w & 0x100 else 1
    if t is None:
        return ()
    t = (a + 8 + (t if up else -t)) & ~3
    return tuple(t + 4 * k for k in range(n))


def pc_relative_data(data, base, text_end):
    """Every word a pc-relative load reads anywhere in the text segment (pc_relative_words)."""
    out = set()
    for a in range(base, text_end - 3, 4):
        out.update(pc_relative_words(struct.unpack_from("<I", data, a - base)[0], a))
    return out


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def text_word(data, off):
    """whether the word at off is text inside a string: its 4 bytes printable (or zero, line breaks) and a word beside it
    too (an ARM instruction's top byte, its condition, is 0xE0 or more when unconditional: such a word is never text;
    Omega Ruby's 00136D04 holds strings that read as a bl)"""
    ok = lambda o: 0 <= o <= len(data) - 4 and all(b == 0 or 0x20 <= b < 0x7F or 9 <= b <= 13 for b in data[o:o + 4])
    return ok(off) and any(b for b in data[off:off + 4]) and (ok(off - 4) or ok(off + 4))


def walk_arm(word, start, limit, text_end, cover, starts=frozenset()):
    """an ARM function from start to its end: (the address after it, its literal pool words)"""
    pool = set()
    reach = start  # the furthest address a branch of this function reaches
    a = start
    while a < text_end:
        if a in pool:
            a += 4
            continue
        if a != start and a in starts:  # another function's start: this one ends before it at the latest
            return a, pool
        cover.add(a)
        w = word(a)
        cond = w >> 28
        if (w & 0x0F000000) == 0x0A000000 and cond != 0xF:  # b
            t = a + 8 + sext(w & 0xFFFFFF, 24) * 4
            prev = word(a - 4) if a - 4 >= start else 0
            after_pop = (prev & 0xFFFF4000) == 0xE8BD4000 and not prev & 0x8000  # pop {..., lr}: frame undone
            to_push = cond == 0xE and start <= t < text_end and word(t) >> 16 == 0xE92D  # an unconditional b to a push: a tail
            # call to another function (DllBattleMatch's 00C01C to 00C110, over the function at 00C028)
            if a < t < limit and not after_pop and not to_push:  # forward, before the next known function: inside
                reach = max(reach, t)  # this one (a loop entered at its test); a b right after a pop of lr is a tail call
            if cond == 0xE and a >= reach:
                break  # an unconditional b no branch of the function jumps past: its end
        elif cond != 0xF and (w & 0x0F7FF000) == 0x071FF000:  # ldr pc, [pc, rN, lsl #2]: a switch, its table after it
            b = a + 8  # (the instruction at a + 4 is the default case, reached when the condition fails)
            while b < text_end and start <= word(b) < text_end and word(b) & 3 == 0 and b not in starts:
                pool.add(b)
                reach = max(reach, word(b))  # each case is code of this function (Omega Ruby's 00232A30: its cases
                b += 4  # from 00232DB8 were split off as functions)
            reach = max(reach, b)
        elif cond != 0xF and (w & 0x0FFFF000) == 0x008FF000:  # add pc, pc, rN, lsl #2: a table of b's after it
            b = a + 8
            while b < text_end and (word(b) & 0x0F000000) == 0x0A000000 and word(b) >> 28 == 0xE:
                reach = max(reach, b + 8 + sext(word(b) & 0xFFFFFF, 24) * 4)
                b += 4
            reach = max(reach, b)
        elif pc_relative_words(w, a):  # a pc-relative load (ldr, ldrh / ldrd, vldr): its literal pool words
            pool.update(pc_relative_words(w, a))  # (vldr's floats too: DllBattle's 0625A8 was walked past them)
            if (w & 0x0F7FF000) == 0x051FF000 and a >= reach:  # ldr pc, [pc, #imm]: a jump through the pool
                break
        elif is_return(w) and a >= reach and not is_call_through(word, start, a):
            break
        elif (w == 0xE1A00000 and a > start or (w & 0xFFFF4000) == 0xE8BD4000 and not w & 0x8000) and a >= reach:
            # nop padding, or pop {..., lr} with no pc (the frame undone): when a push follows (after nop padding), the
            # function falls into the next one, a tail call by placement (DllField's 0668A8 into 0668B0, Omega Ruby's
            # 004C4294 into 004C42CC)
            b = a if w == 0xE1A00000 else a + 4
            while b < text_end and word(b) == 0xE1A00000:
                b += 4
            if b < text_end and word(b) >> 16 == 0xE92D:
                return b, pool
        a += 4
    return a + 4, pool


def thumb_invalid(h, h2):
    """a halfword ARMv6K's Thumb (Thumb-1 and the bl / blx pair) does not have: Thumb-2 encodings, cbz / cbnz, it and
    the hints, udf, bkpt, bx / blx with low bits set (h2: the next halfword, for the bl / blx pair)"""
    if 0xF000 <= h < 0xF800:
        return not (h2 >> 11 == 0x1F or h2 >> 11 == 0x1D and not h2 & 1)
    return (h >= 0xE800 or 0xDE00 <= h < 0xDF00 or h >> 8 in (0xB1, 0xB3, 0xB7, 0xB8, 0xB9, 0xBB, 0xBE, 0xBF)
            or h >> 8 == 0xB6 and not 0xB650 <= h < 0xB680 or 0xBA80 <= h < 0xBAC0
            or h >> 8 == 0x47 and h & 7)


def text_like(data, off):
    """whether the 8 bytes at off are printable text, line breaks or zeros (strings placed in the text segment)"""
    return all(b == 0 or 0x20 <= b < 0x7F or 9 <= b <= 13 for b in data[off:off + 8])


def arm_plausible(w):
    """an unconditional ARM word that can open a function: not a coprocessor instruction other than VFP (cp10, cp11) or
    the system one (cp15), which Thumb or data read as ARM gives (Omega Ruby's 001000F0, mcr p1)"""
    if w >> 28 != 0xE:
        return False
    if (w >> 25) & 7 == 6 or (w >> 24) & 0xF == 0xE:  # ldc / stc, cdp / mcr / mrc
        return (w >> 8) & 0xF in (10, 11, 15)
    return True


ARM_DECODER = Cs(CS_ARCH_ARM, CS_MODE_ARM)


def arm_valid(data, base, start, text_end):
    """whether the words from start read as the opening of an ARM function: up to its first return, at most 8, each one
    an instruction (capstone decodes it) and no coprocessor one other than VFP (cp10, cp11) or the system one (cp15).
    A data table whose first word reads as an instruction fails within a few words (Omega Ruby's 0016FDFC, 1/9 and the
    other reciprocals; 00197384, a table before the function at 001973A0)"""
    for a in range(start, min(text_end, start + 32), 4):
        w = struct.unpack_from("<I", data, a - base)[0]
        coprocessor = w >> 28 != 0xF and ((w >> 25) & 7 == 6 or (w >> 24) & 0xF == 0xE)  # ldc / stc, cdp / mcr / mrc
        if coprocessor and (w >> 8) & 0xF not in (10, 11, 15) or \
                not any(True for _ in ARM_DECODER.disasm(data[a - base:a - base + 4], a)):
            return False
        if is_return(w):
            return True
    return True


def thumb_valid(half, start, text_end, starts=frozenset(), strict=False):
    """whether the code at start reads as a whole Thumb function: every block reached from it (following branches, so
    data a branch jumps over, such as a string an adr names, is never read: Omega Ruby's 00103780) is valid ARMv6K
    Thumb, ends on a return, a branch or a known start, and stays within 4 KB; at least one return or one call (a function
    may end on a call that does not return). Data read as Thumb meets an invalid halfword. strict also keeps every
    branch before the next known start. strict, for a start that opens on no push: at least 3 instructions, no movs r0, r0, only
    bx lr or pop {pc} as returns, and no block ending by falling into the 4 KB limit (switch tables and data read as Thumb
    give short runs ending in b or bx rN: Omega Ruby's 001060A6, 004174E4, the offset table at 001AE404)"""
    todo, seen = [start], set()
    count = returns = calls = 0
    end = min(text_end, start + 0x1000)
    if strict:  # its branches stay before the next known function (a run whose b leads into another is not one)
        end = next((b for b in range(start + 2, end, 2) if b in starts), end)
    while todo:
        a = todo.pop()
        while True:
            if a in seen:
                break
            if a != start and a in starts:  # falls into the next function (before the window test: that is its end)
                break
            if not start <= a < end:
                if strict:
                    return False
                break
            seen.add(a)
            h = half(a)
            h2 = half(a + 2) if a + 2 < text_end else 0
            if thumb_invalid(h, h2) or strict and h == 0:
                return False
            count += 1
            if 0xF000 <= h < 0xF800:
                calls += 1
                a += 4
                continue
            if h >> 12 == 0xD and (h >> 8) & 0xF < 0xE:  # b<cond>: both ways
                t = a + 4 + sext(h & 0xFF, 8) * 2
                if strict and not start <= t < end:
                    return False
                todo.append(t)
            elif h >> 11 == 0x1C:  # b: only its target
                t = a + 4 + sext(h & 0x7FF, 11) * 2
                if start <= t < end:
                    todo.append(t)
                elif strict:
                    return False  # a short run ending in a b out of it: a table or data
                break
            elif h == 0x4770 or h >> 8 == 0xBD:
                returns += 1
                break
            elif (h & 0xFF87) == 0x4700:
                if strict:
                    return False
                returns += 1
                break
            a += 2
    # a function may end on calls that do not return (an error routine: Omega Ruby's 00103780 ends on blx 002FED34)
    return (returns > 0 or calls > 0) and count >= (3 if strict else 1)


def walk_thumb(half, start, limit, text_end, cover, starts=frozenset()):
    """a Thumb function (ARMv6K: Thumb-1 and the bl / blx pair) from start to its end: (the address after it, its
    literal pool words, the Thumb functions it calls with bl, the ARM ones it calls with blx)"""
    pool, thumb_calls, arm_calls = set(), set(), set()
    reach = a = start
    while a < text_end:
        if a in pool:
            a += 4
            continue
        if a != start and a in starts:
            return a, pool, thumb_calls, arm_calls
        cover.add(a)
        h = half(a)
        if 0xF000 <= h < 0xF800 and a + 2 < text_end:  # bl / blx: two halfwords
            h2 = half(a + 2)
            t = a + 4 + sext(((h & 0x7FF) << 12) | ((h2 & 0x7FF) << 1), 23)
            if h2 >> 11 == 0x1F:
                thumb_calls.add(t)
            elif h2 >> 11 == 0x1D:
                arm_calls.add(t & ~3)
            a += 4
            continue
        if h >> 11 == 0x09:  # ldr rX, [pc, #imm8 * 4]
            pool.add(((a + 4) & ~3) + (h & 0xFF) * 4)
        elif h >> 12 == 0xD and (h >> 8) & 0xF < 0xE:  # b<cond>
            t = a + 4 + sext(h & 0xFF, 8) * 2
            if a < t < limit:
                reach = max(reach, t)
        elif h >> 11 == 0x1C:  # b
            t = a + 4 + sext(h & 0x7FF, 11) * 2
            after_pop = a - 2 >= start and half(a - 2) >> 8 == 0xBC  # pop {...} with no pc: a tail call
            if a < t < limit and not after_pop:
                reach = max(reach, t)
            if a >= reach:
                break
        elif (h == 0x4770 or h >> 8 == 0xBD or (h & 0xFF87) == 0x4700) and a >= reach:  # bx lr, pop {pc}, bx rN
            break
        a += 2
    return a + 2, pool, thumb_calls, arm_calls


def flow_pass(data, base, text_end, known, thumb=frozenset(), cover=None, ends=None):
    """Partitions the text segment into functions (see the module notes): (every start found, known ones included; the
    Thumb ones among them). thumb: the known starts that are Thumb. cover, when given, receives every address walked as
    an ARM instruction; ends, when given, receives each function's end (the address after its last instruction, its
    trailing literal pool and the data after it left out)."""
    ends = {} if ends is None else ends
    cover = set() if cover is None else cover
    cover_thumb = set()
    word = lambda a: struct.unpack_from("<I", data, a - base)[0]
    half = lambda a: struct.unpack_from("<H", data, a - base)[0]
    starts = set(a for a in known if base <= a < text_end)
    thumb = set(a for a in thumb if a in starts)
    pool_words = pc_relative_data(data, base, text_end)
    called = sorted(starts)  # the starts known before the pass bound the forward branches
    queue = [k for k in called if k % 4 == 0 or k in thumb]
    seen = set()
    thumb_pool = set()

    def add(t, is_thumb):
        if base <= t < text_end:
            if t in starts:
                is_thumb = t in thumb  # a start keeps the instruction set it was first known with
            starts.add(t)
            if is_thumb:
                thumb.add(t)
            queue.append(t)

    while queue:
        start = queue.pop()
        if start in seen:
            continue
        seen.add(start)
        i = bisect.bisect_right(called, start)
        limit = called[i] if i < len(called) else text_end
        if start in thumb:
            a, pool, thumb_calls, arm_calls = walk_thumb(half, start, limit, text_end, cover_thumb, starts)
            ends[start] = min(a, text_end)
            for t in thumb_calls:
                add(t, True)
            for t in arm_calls:
                add(t, False)
        else:
            walked = set()
            a, pool = walk_arm(word, start, limit, text_end, walked, starts)
            ends[start] = min(a, text_end)
            cover.update(b for b in walked if not text_word(data, b - base))
        # what follows: its pool, padding (zeros, Thumb nops), address tables; then an unconditional ARM instruction (a
        # function never opens on a conditional one: what follows a pool and does not is data, tables, floats) or a Thumb
        # push {..., lr}
        if start in thumb:
            # after a Thumb function: its pool (and the pools of the other Thumb functions walked so far), padding
            # (zeros, mov r8, r8), address tables; then a known start, ARM code (below), or else Thumb code (a block
            # reached only through the exception tables opens on no push: Omega Ruby's 0010255A)
            thumb_pool.update(pool)
            while a < text_end:
                if a in thumb_pool or (a - 2) in thumb_pool and a % 4:
                    a += 2
                    continue
                h = half(a)
                if h in (0, 0x46C0):
                    a += 2
                elif a in starts:
                    add(a, a in thumb)
                    break
                elif a % 4 == 0 and word(a) == 0xE1A00000:
                    a += 4  # ARM nop padding
                elif a % 4 == 0 and (word(a) >> 16 == 0xE92D or word(a) >> 28 == 0xE and a + 4 < text_end and
                                     word(a + 4) >> 28 == 0xE) and arm_valid(data, base, a, text_end):
                    # an ARM push, or two unconditional ARM words in a row (as Thumb, two b's in a row: not code a
                    # compiler writes): ARM (Omega Ruby's 00301FB4, mov r1, #...; mov r0, r0)
                    add(a, False)
                    break
                elif a % 4 == 0 and base <= word(a) < text_end:
                    a += 4
                elif not text_like(data, a - base) and thumb_valid(half, a, text_end, starts, half(a) >> 9 != 0x5A):
                    add(a, True)
                    break
                else:
                    a += 2  # data between Thumb functions (Omega Ruby's 00100072, 00100188: Thumb-2 encodings)
            continue
        while a < text_end:
            if a % 4:
                if half(a) >> 8 == 0xB5:
                    add(a, True)
                    break
                a += 2
                continue
            w = word(a)
            nxt = word(a + 4) if a + 4 < text_end else 0
            if a not in pool and (w & 0xFF00) == 0xB500 and not base <= nxt < text_end and (
                    a in pool_words or base <= w < text_end) and thumb_valid(half, a, text_end, starts):
                # a Thumb push where the ARM scan of the whole text saw a pool word (Thumb read as ARM there is noise),
                # or whose two halfwords read as an address (X's 002FEB88: push, then movs r6, r2 = 0x0016B5F7); an
                # address table has addresses on both sides, this one is followed by code; and it must read as Thumb
                # (X's 0045ACAC, 0x005EB5C0, is a data address before the ARM function at 0045ACB0)
                add(a, True)
                break
            elif a in pool or w == 0 or base <= w < text_end or w == 0xE1A00000 and a not in starts:
                a += 4  # (a nop no call reaches is padding before the next function: Omega Ruby's 0015D828)
            elif a in pool_words:
                a += 4
            elif arm_plausible(w) and (a in starts or arm_valid(data, base, a, text_end)):
                add(a, False)
                break
            elif ((w & 0xFF00) == 0xB500 or not arm_plausible(w)) and not text_like(data, a - base) and \
                    thumb_valid(half, a, text_end, starts, (w & 0xFE00) != 0xB400):
                # a Thumb push, or a word that is no ARM instruction opening a function but reads as a whole Thumb
                # function (Omega Ruby's 001000EC, a Thumb leaf after an ARM one); text reads as valid Thumb (bytes
                # below 0x80) and is left out (Omega Ruby's path string before 003AAF8C)
                add(a, True)
                break
            else:
                a += 4
    # a start the flow pass found that a conditional branch of the ARM function before it reaches is part of that
    # function (its walk ended early at a return the branch jumps over): merged, until none is left
    cond_branches = []
    for a in range(base, text_end - 3, 4):
        w = word(a)
        if (w & 0x0F000000) == 0x0A000000 and w >> 28 < 0xE:
            cond_branches.append((a, a + 8 + sext(w & 0xFFFFFF, 24) * 4))
    while True:
        ordered = sorted(starts)
        merged = set()
        for a, t in cond_branches:
            if t > a and t in starts and t not in known and t not in thumb:
                i = bisect.bisect_right(ordered, a)
                if i < len(ordered) and ordered[i] == t and ordered[i - 1] not in thumb:
                    merged.add(t)
        if not merged:
            return starts, thumb
        for t in sorted(merged, reverse=True):  # the function before it now ends where the merged piece ended
            p = ordered[bisect.bisect_left(ordered, t) - 1]
            ends[p] = max(ends.get(p, t), ends.pop(t, t))
        starts -= merged


if __name__ == "__main__":
    main()
