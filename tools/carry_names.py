#!/usr/bin/env python3
"""Omega Ruby's function names carried to X through the pairs of shared_functions.py (step 4 of
docs/RECOMPILATION_PLAN.md §10): a name found in one game names its copy in the other.

  carry_names.py <function_names.tsv> <modules.tsv> <shared_functions.tsv> <out.tsv>

function_names.tsv is Pomegrade's (tools/remake/ghidra/function_names.tsv: address, name, role, read|guess), read where it
is, not copied (docs/RECOMPILATION_PLAN.md §5: one copy). Its module addresses are the ones Pomegrade's
tools/remake/prototype/cro_link.py gives when it links every module in name order from 0x10000000; modules.tsv is that
tool's map (module, base, size), run on Omega Ruby's modules, which turns them back into a module and its file offset.

out.tsv (names and places only): x place, name, oras place, the pair's kind and similarity, the name's own status
(read / guess). A name carried through a "near" or "repeated" pair is less sure than through an "identical" one: the kind
says which.
"""
import argparse


def main():
    p = argparse.ArgumentParser()
    p.add_argument("names")
    p.add_argument("modules")
    p.add_argument("shared")
    p.add_argument("out")
    a = p.parse_args()
    bases = []
    for line in open(a.modules):
        f = line.rstrip("\n").split("\t")
        if len(f) == 3 and f[0]:
            bases.append((int(f[1], 16), int(f[2], 16), f[0]))
    pairs = {}
    for line in open(a.shared):
        if not line.startswith("#"):
            o, x, kind, r = line.rstrip("\n").split("\t")
            pairs[o] = (x, kind, r)
    carried = missing = 0
    kinds = {}
    with open(a.out, "w") as out:
        out.write("# x place\tname\toras place\tpair kind\tsimilarity\tname status\n")
        for line in open(a.names):
            if line.startswith("#") or not line.strip():
                continue
            f = line.rstrip("\n").split("\t")
            addr, name, status = int(f[0], 16), f[1], f[-1]
            place = "%08X" % addr
            if addr >= 0x10000000:
                place = next(("%s+%06X" % (m, addr - b) for b, s, m in bases if b <= addr < b + s), None)
            if place in pairs:
                x, kind, r = pairs[place]
                out.write("%s\t%s\t%s\t%s\t%s\t%s\n" % (x, name, place, kind, r, status))
                carried += 1
                kinds[kind] = kinds.get(kind, 0) + 1
            else:
                missing += 1
    print("%d names carried to X (%s), %d with no pair" % (carried, ", ".join("%s %d" % kv for kv in sorted(kinds.items())),
                                                          missing))


if __name__ == "__main__":
    main()
