#!/usr/bin/env python3
"""Step 4 of docs/RECOMPILATION_PLAN.md §7: which functions Omega Ruby and X share. Pairs each function of one game's list
(symbols/<game>_functions.tsv) with its copy in the other, so a name found in one game carries to the other.

  shared_functions.py <oras code.bin> <x code.bin> <oras_functions.tsv> <x_functions.tsv> <out.tsv> [--near 0.9]
                      [--cro <oras cro dir> <x cro dir> <oras_cro_functions.tsv> <x_cro_functions.tsv>]

The .code and, with --cro, the modules' code (cro_functions.py) form one pool per game: a function may have moved between
the .code and a module from one game to the other. A function's extent is its start up to the next entry of its own list
in the same place (at most 16 KB): a missed start leaves two functions in one extent, which only makes a match harder.
Before comparing, the words that name an address are set aside, since they move between the games:
  - the offset of a bl / blx (direct calls);
  - in the .code, a word whose value lies in the program's address range, base to 0x01000000 (literal pool addresses of
    code, data and bss; a guess wide enough for both games, the .bss ends below it in both: inferred from the exheaders);
  - in a module, every word its relocation tables name (the loader rewrites them: imports and internal addresses).
pc-relative loads and branches inside a function are kept: their offsets stay equal when the function is unchanged.

  identical  the masked words equal, and that content found once in each game
  repeated   the same content found several times in a game (small stubs, template copies) and the same number of times
             in the other: paired in address order (the bodies are equal; whether the names are is not known)
  ambiguous  the same content found a different number of times in the two games: not paired
  prefix     not identical, the same first 16 masked words found once in each game, and the shorter extent the start of
             the longer: the same code, the two lists cutting it at different places
  near       not identical, its candidate either lying between the same two identical pairs in both games (same order) or
             the X function with the same unique first 16 words, and the best of them by difflib ratio over the masked
             words, at least --near: the same function lightly changed (or cut differently and changed); a gap's two
             anchors are in the same place (the .code or one module) in each game; never for a function under 4 words
             or one whose exact content is in the other game
  veneer     a module's import veneers are left out of every count
  rest       one game only, or changed beyond --near, or with no candidate (no anchored gap, no unique start)

out.tsv (addresses only, no game data, CLAUDE.md §1): oras place, x place, kind, similarity; a place is a .code address
(00123456) or a module and its file offset (DllField+01A2B0).
Needs capstone (cro_functions.py imports function_list.py) only with --cro.
"""
import argparse
import bisect
import collections
import difflib
import hashlib
import struct

MAX_EXTENT = 16 * 1024
PREFIX = 16  # words
NEAR_MIN = 4  # words: a shorter function is paired only when identical


def masked(data, start, end, is_address):
    words = []
    for off in range(start, end, 4):
        w = struct.unpack_from("<I", data, off)[0]
        if is_address(off, w):
            w = 0xADD2E55  # an address: set aside
        elif (w & 0x0E000000) == 0x0A000000 and ((w >> 24) & 1 or (w >> 28) == 0xF):  # bl (any cond) / blx imm
            w &= 0xFF000000
        words.append(w)
    return tuple(words)


def load(code_path, list_path, base, cro=None):
    """{place: masked words}; a place is ("", address) in the .code, (module, file offset) in a module"""
    data = open(code_path, "rb").read()
    starts = sorted(int(line.split("\t")[0], 16) for line in open(list_path) if not line.startswith("#"))
    end = base + len(data)
    funcs = {}
    for i, s in enumerate(starts):
        e = min(starts[i + 1] if i + 1 < len(starts) else end, s + MAX_EXTENT, end)
        funcs[("", s)] = masked(data, s - base, e - base, lambda off, w: base <= w < 0x01000000)
    if cro:
        cro_dir, cro_list = cro
        mods = {m.name: m for m in cro_functions.load(cro_dir)}
        by_mod = collections.defaultdict(list)
        for line in open(cro_list):
            if not line.startswith("#"):
                name, off, _ = line.split("\t")
                by_mod[name].append(int(off, 16))
        for name, offs in by_mod.items():
            m = mods[name]
            offs.sort()
            for i, s in enumerate(offs):
                e = min(offs[i + 1] if i + 1 < len(offs) else m.code_end, s + MAX_EXTENT, m.code_end)
                funcs[(name, s)] = masked(m.data, s, e, lambda off, w, r=m.relocated: off in r)
    return sorted(funcs), funcs


def place(k):
    return "%08X" % k[1] if not k[0] else "%s+%06X" % k


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
    p.add_argument("--cro", nargs=4, metavar=("ORAS_DIR", "X_DIR", "ORAS_LIST", "X_LIST"))
    a = p.parse_args()
    if a.cro:
        global cro_functions
        import cro_functions
    o_starts, o = load(a.oras_code, a.oras_list, a.base, a.cro and (a.cro[0], a.cro[2]))
    x_starts, x = load(a.x_code, a.x_list, a.base, a.cro and (a.cro[1], a.cro[3]))

    o_by = collections.defaultdict(list)
    x_by = collections.defaultdict(list)
    # a module's import veneers (ldr pc, [pc, #-4] and the imported address: 18,923 in Omega Ruby's modules) are not
    # functions of the game: their pair follows from the import's name, not from their two words
    veneer = (0xE51FF004, 0xADD2E55)
    o = {k: w for k, w in o.items() if w != veneer}
    x = {k: w for k, w in x.items() if w != veneer}
    o_starts = [k for k in o_starts if k in o]
    x_starts = [k for k in x_starts if k in x]
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
        elif len(os_) == len(xs):
            for oa, xa in zip(sorted(os_), sorted(xs)):
                pairs[oa] = (xa, "repeated", 1.0)
        else:
            ambiguous_o += len(os_)

    # near: between consecutive identical pairs that keep their order in both games
    anchors = sorted((oa, xa) for oa, (xa, k, _) in pairs.items() if k == "identical")
    ordered = [anchors[0]] if anchors else []
    for oa, xa in anchors[1:]:
        if xa > ordered[-1][1]:
            ordered.append((oa, xa))
    paired_x = {xa for xa, _, _ in pairs.values()}
    near = {}
    gap_candidates = {}
    for (o0, x0), (o1, x1) in zip(ordered, ordered[1:]):
        if o0[0] != o1[0] or x0[0] != x1[0]:
            continue  # a gap is inside one place (the .code, one module) in both games
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
        if oa in pairs or oa in prefix or len(o[oa]) < NEAR_MIN or digest(o[oa]) in x_by:
            continue  # too short for a ratio to mean anything, or its exact content is in X (ambiguous, not near)
        cands = list(gap_candidates.get(oa, ()))
        w = o[oa]
        if len(w) >= PREFIX and o_pre[w[:PREFIX]] == 1 and w[:PREFIX] in x_by_pre:
            cands.append(x_by_pre[w[:PREFIX]])
        if cands:
            best = (0.0, None)
            for xa in cands:
                if xa in taken or len(x[xa]) < NEAR_MIN:
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
        f.write("# oras place\tx place\tkind\tsimilarity\n")
        for oa in sorted(pairs):
            xa, kind, r = pairs[oa]
            f.write("%s\t%s\t%s\t%.3f\n" % (place(oa), place(xa), kind, r))
    for label, part in ((".code", lambda k: not k[0]), ("modules", lambda k: k[0])):
        ok = [k for k in o if part(k)]
        if not ok:
            continue
        kinds = collections.Counter(pairs[k][1] for k in ok if k in pairs)
        by_place = collections.Counter("code" if not pairs[k][0][0] else "module" for k in ok if k in pairs)
        size = sum(len(o[k]) for k in ok) * 4
        paired = sum(len(o[k]) for k in ok if k in pairs) * 4
        print("Omega Ruby %s: %d functions; %s; X copy in the .code %d, in a module %d; paired %d of %d bytes (%d %%)"
              % (label, len(ok), ", ".join("%s %d" % kv for kv in sorted(kinds.items())), by_place["code"],
                 by_place["module"], paired, size, 100 * paired // size))
    print("ambiguous (same content a different number of times) %d in Omega Ruby; X with no Omega Ruby copy: %d of %d"
          % (ambiguous_o, len(x) - len({v[0] for v in pairs.values()}), len(x)))


if __name__ == "__main__":
    main()
