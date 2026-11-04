#!/usr/bin/env python3
"""Step 4 of docs/RECOMPILATION_PLAN.md §7: which functions Omega Ruby and X share. Pairs each function of one game's list
(symbols/<game>_functions.tsv) with its copy in the other, so a name found in one game carries to the other.

  shared_functions.py <oras code.bin> <x code.bin> <oras_functions.tsv> <x_functions.tsv> <out.tsv> [--near 0.9]

A function's extent is its start up to the next entry of its own list (at most 16 KB): the lists are incomplete
(function_list.py), so an extent may hold several real functions, which only makes a match harder, never wrong.
Before comparing, the words that name an address are set aside, since they move between the games:
  - the offset of a bl / blx (direct calls);
  - a word whose value lies in the program's address range, base to 0x01000000 (literal pool addresses of code, data and
    bss; a guess wide enough for both games, the .bss ends below it in both: inferred from the exheaders' sizes).
pc-relative loads and branches inside a function are kept: their offsets stay equal when the function is unchanged.

  identical  the masked words equal, and that content found once in each game (a content found several times in a game,
             e.g. the same small stub at several addresses, is counted apart: "ambiguous", not paired)
  prefix     not identical, the same first 16 masked words found once in each game, and the shorter extent the start of
             the longer: the same code, the two lists cutting it at different places
  near       not identical, its candidate either lying between the same two identical pairs in both games (same order) or
             the X function with the same unique first 16 words, and the best of them by difflib ratio over the masked
             words, at least --near: the same function lightly changed (or cut differently and changed)
  rest       one game only, or changed beyond --near, or with no candidate (no anchored gap, no unique start)

out.tsv (addresses only, no game data, CLAUDE.md §1): oras address, x address, kind, similarity.
Python only (no capstone).
"""
import argparse
import bisect
import collections
import difflib
import hashlib
import struct

MAX_EXTENT = 16 * 1024
PREFIX = 16  # words


def load(code_path, list_path, base):
    data = open(code_path, "rb").read()
    starts = sorted(int(line.split("\t")[0], 16) for line in open(list_path) if not line.startswith("#"))
    end = base + len(data)
    funcs = {}
    for i, s in enumerate(starts):
        e = min(starts[i + 1] if i + 1 < len(starts) else end, s + MAX_EXTENT, end)
        words = []
        for off in range(s - base, e - base, 4):
            w = struct.unpack_from("<I", data, off)[0]
            if (w & 0x0E000000) == 0x0A000000 and ((w >> 24) & 1 or (w >> 28) == 0xF):  # bl (any cond) / blx imm
                w &= 0xFF000000
            elif base <= w < 0x01000000:
                w = 0xADD2E55  # an address: set aside
            words.append(w)
        funcs[s] = tuple(words)
    return starts, funcs


def digest(words):
    return hashlib.blake2b(struct.pack("<%dI" % len(words), *words), digest_size=16).digest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("oras_code")
    p.add_argument("x_code")
    p.add_argument("oras_list")
    p.add_argument("x_list")
    p.add_argument("out")
    p.add_argument("--base", type=lambda s: int(s, 0), default=0x100000)
    p.add_argument("--near", type=float, default=0.9)
    a = p.parse_args()
    o_starts, o = load(a.oras_code, a.oras_list, a.base)
    x_starts, x = load(a.x_code, a.x_list, a.base)

    o_by = collections.defaultdict(list)
    x_by = collections.defaultdict(list)
    for s, w in o.items():
        o_by[digest(w)].append(s)
    for s, w in x.items():
        x_by[digest(w)].append(s)
    pairs = {}
    ambiguous_o = 0
    for h, os_ in o_by.items():
        xs = x_by.get(h)
        if not xs:
            continue
        if len(os_) == 1 and len(xs) == 1:
            pairs[os_[0]] = (xs[0], "identical", 1.0)
        else:
            ambiguous_o += len(os_)

    # near: between consecutive identical pairs that keep their order in both games
    anchors = sorted((oa, xa) for oa, (xa, _, _) in pairs.items())
    ordered = [anchors[0]] if anchors else []
    for oa, xa in anchors[1:]:
        if xa > ordered[-1][1]:
            ordered.append((oa, xa))
    paired_x = {xa for xa, _, _ in pairs.values()}
    near = {}
    gap_candidates = {}
    for (o0, x0), (o1, x1) in zip(ordered, ordered[1:]):
        og = [s for s in o_starts[bisect.bisect_right(o_starts, o0):bisect.bisect_left(o_starts, o1)] if s not in pairs]
        xg = [s for s in x_starts[bisect.bisect_right(x_starts, x0):bisect.bisect_left(x_starts, x1)] if s not in paired_x]
        for oa in og:
            gap_candidates[oa] = xg
    # a second anchor: the same first PREFIX words, found once in each game (a function whose gap is not anchored)
    o_pre = collections.Counter(w[:PREFIX] for w in o.values() if len(w) >= PREFIX)
    x_pre = collections.Counter(w[:PREFIX] for w in x.values() if len(w) >= PREFIX)
    x_by_pre = {w[:PREFIX]: s for s, w in x.items() if len(w) >= PREFIX and x_pre[w[:PREFIX]] == 1}
    prefix = {}
    taken = set(paired_x)
    for oa in o_starts:
        if oa in pairs:
            continue
        w = o[oa]
        xa = x_by_pre.get(w[:PREFIX]) if len(w) >= PREFIX and o_pre[w[:PREFIX]] == 1 else None
        if xa is not None and xa not in taken:
            m = min(len(w), len(x[xa]))
            if w[:m] == x[xa][:m]:
                prefix[oa] = (xa, "prefix", m / max(len(w), len(x[xa])))
                taken.add(xa)
    for oa in o_starts:
        if oa in pairs or oa in prefix:
            continue
        cands = list(gap_candidates.get(oa, ()))
        w = o[oa]
        if len(w) >= PREFIX and o_pre[w[:PREFIX]] == 1 and w[:PREFIX] in x_by_pre:
            cands.append(x_by_pre[w[:PREFIX]])
        if cands:
            best = (0.0, None)
            for xa in cands:
                if xa in taken:
                    continue
                sm = difflib.SequenceMatcher(None, o[oa], x[xa], autojunk=False)
                if sm.real_quick_ratio() < a.near or sm.quick_ratio() < a.near:
                    continue
                r = sm.ratio()
                if r > best[0]:
                    best = (r, xa)
            if best[1] is not None and best[0] >= a.near:
                near[oa] = (best[1], "near", best[0])
                taken.add(best[1])
    pairs.update(prefix)
    pairs.update(near)

    with open(a.out, "w") as f:
        f.write("# oras address\tx address\tkind\tsimilarity\n")
        for oa in sorted(pairs):
            xa, kind, r = pairs[oa]
            f.write("%08X\t%08X\t%s\t%.3f\n" % (oa, xa, kind, r))
    ident = len(pairs) - len(near) - len(prefix)
    ow = sum(len(o[s]) for s in pairs) * 4
    print("Omega Ruby %d functions, X %d" % (len(o), len(x)))
    print("identical %d, prefix %d, near %d (>= %.2f), ambiguous (same content several times) %d in Omega Ruby" %
          (ident, len(prefix), len(near), a.near, ambiguous_o))
    print("Omega Ruby only or changed: %d; X only or changed: %d" % (len(o) - len(pairs) - ambiguous_o,
                                                                    len(x) - len(pairs)))
    print("paired code in Omega Ruby: %d of %d bytes in the listed extents" % (ow, sum(len(w) for w in o.values()) * 4))


if __name__ == "__main__":
    main()
