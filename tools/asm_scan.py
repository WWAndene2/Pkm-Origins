#!/usr/bin/env python3
"""Searches of the game's ARM code for what hand-written code does that the translation must know about
(docs/RECOMPILATION_PLAN.md §10; the video decoder's work, 6 November).

  asm_scan.py <code.bin> hand-calls [--from A --to B]       the calls made by hand: lr set (add lr, pc, #n / mov lr, pc)
                                                              then a jump, by the form of the jump (add pc, bcc, b, bx...)
  asm_scan.py <code.bin> refs <address> [--from A --to B]   what can lead to an address: pc-relative adds that compute
                                                              it, tables of offsets from pc that hold it, literal words
  asm_scan.py <code.bin> literals <functions.tsv>           the addresses inside functions a literal pool holds (a word
                                                              ldr rX, [pc, #n] loads): tools/translate_program.py makes
                                                              them entries

--from / --to: the range searched (default the whole .code). The .code is loaded at 0x100000. Reads the game's code;
writes nothing.
"""
import argparse
import bisect
import collections
import os
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
BASE = 0x100000


def hand_calls(code, lo, hi):
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    kinds, examples = collections.Counter(), {}
    for off in range(lo - BASE, hi - BASE - 4, 4):
        w = struct.unpack_from("<I", code, off)[0]
        if (w & 0xFFFFF000) != 0xE28FE000 and w != 0xE1A0E00F:  # add lr, pc, #imm / mov lr, pc (always)
            continue
        kind = "no jump within 5 instructions"
        for i in list(md.disasm(code[off + 4:off + 24], BASE + off + 4))[:5]:
            m, o = i.mnemonic, i.op_str
            if m.startswith("b") and not m.startswith("bic"):
                kind = "%s %s" % (m, "register" if not o.startswith("#") else "immediate")
                break
            if o.startswith("pc,") and m[:3] in ("add", "ldr", "mov", "sub"):
                kind = m[:3] + " pc"
                break
            if m.startswith(("pop", "ldm")) and "pc" in o:
                kind = "pop pc"
                break
        kinds[kind] += 1
        examples.setdefault(kind, BASE + off)
    for k, n in kinds.most_common():
        print("%5d  %-28s first at %08X" % (n, k, examples[k]))


def refs(code, target, lo, hi):
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    for i in md.disasm(code[lo - BASE:hi - BASE], lo):
        s = i.op_str
        if ", pc, #" in s and i.mnemonic[:3] in ("add", "sub"):
            imm = int(s.split("#")[1], 0)
            v = i.address + 8 + (imm if i.mnemonic.startswith("add") else -imm)
            if v == target:
                print("computed from pc at %08X: %s %s" % (i.address, i.mnemonic, s))
            for n in range(128):  # (v as a table base: a word w with v + w the target)
                o = v - BASE + 4 * n
                if 0 <= o < len(code) - 4 and (v + struct.unpack_from("<I", code, o)[0]) & 0xFFFFFFFF == target:
                    print("table of offsets at %08X (entry %d), its base set at %08X: %s %s" % (v, n, i.address,
                                                                                               i.mnemonic, s))
    for off in range(0, len(code) - 4, 4):
        if struct.unpack_from("<I", code, off)[0] == target:
            print("literal word at %08X" % (BASE + off))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("code")
    p.add_argument("what", choices=("hand-calls", "refs", "literals"))
    p.add_argument("arg", nargs="?")
    p.add_argument("--from", dest="lo", type=lambda x: int(x, 16))
    p.add_argument("--to", dest="hi", type=lambda x: int(x, 16))
    a = p.parse_args()
    code = open(a.code, "rb").read()
    lo, hi = a.lo or BASE, a.hi or BASE + len(code)
    if a.what == "hand-calls":
        hand_calls(code, lo, hi)
    elif a.what == "refs":
        refs(code, int(a.arg, 16), lo, hi)
    else:
        import translate_program
        rows = [l.split("\t") for l in open(a.arg) if l.strip() and not l.startswith("#")]
        starts = sorted(int(r[0], 16) for r in rows)
        size = {int(r[0], 16): int(r[4]) for r in rows}
        arm = {int(r[0], 16) for r in rows if r[3] == "arm"}
        found = sorted(translate_program.literal_code_addresses(code, BASE, starts, size, arm))
        for t in found:
            f = starts[bisect.bisect(starts, t) - 1]
            print("%08X  inside %08X" % (t, f))
        print("%d addresses" % len(found), file=sys.stderr)


if __name__ == "__main__":
    main()
