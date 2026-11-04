#!/usr/bin/env python3
"""Step 4 of docs/RECOMPILATION_PLAN.md §8: which functions Omega Ruby and X share. Pairs each function of one game's list
(symbols/<game>_functions.tsv) with its copy in the other, so a name found in one game carries to the other.

  shared_functions.py <oras code.bin> <x code.bin> <oras_functions.tsv> <x_functions.tsv> <out.tsv> [--near 0.9]
                      [--cro <oras cro dir> <x cro dir> <oras_cro_functions.tsv> <x_cro_functions.tsv>]

The .code and, with --cro, the modules' code (cro_functions.py) form one pool per game: a function may have moved between
the .code and a module from one game to the other. A function's extent is its start up to the next entry of its own list
in the same place, as the list's size column gives it (its own code: the data after it left out; at most 16 KB).
Before comparing, the words that name an address are set aside, since they move between the games:
  - the offset of a bl / blx (direct calls);
  - in the .code, a word whose value lies in the program's address range, base to 0x01000000 (literal pool addresses of
    code, data and bss; a guess wide enough for both games, the .bss ends below it in both: inferred from the exheaders);
  - in a module, every word its relocation tables name (the loader rewrites them: imports and internal addresses).
pc-relative loads and branches inside a function are kept: their offsets stay equal when the function is unchanged.

  identical  the masked words equal, and that content found once in each game
  call       a function called at the same place of two paired functions, both not paired yet, with bodies at least
             CALL_MIN alike, the X one most of its callers name
  -body      after a kind: the same body (identical or repeated), but a call that leads to other functions in the two
             games (a real difference: Omega Ruby's 00355EB8 calls 001397B8, X's copy calls a different function); a
             call, near or prefix pair whose calls contradict the other pairs is removed instead
  repeated   one of several copies of a same content in a game (small stubs, template copies), told apart by a caller
             (check_calls), or the last unpaired copy in each game
  ambiguous  copies of a same content no caller tells apart: not paired
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
Needs capstone (function_list.py imports it).
"""
import argparse
import bisect
import collections
import difflib
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import function_list  # noqa: E402

MAX_EXTENT = 16 * 1024
PREFIX = 16  # words
NEAR_MIN = 4  # words: a shorter function is paired only when identical
# a pair made by a call needs this much of its bodies alike: measured 4 November, the pairs below 0.5 had a call that
# contradicted the other pairs 16 to 39 % of the time, those from 0.9 under 1 %
CALL_MIN = 0.5


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


def thumb_scan(data, start, end, is_address, at, resolve):
    """a Thumb function as (masked halfwords, calls): the bl / blx pairs keep their opcode bits only; a literal pool word
    its ldr read that is an address is set aside; calls as calls_in gives them (index in halfwords)"""
    hs = [struct.unpack_from("<H", data, o)[0] for o in range(start, end - 1, 2)]
    pool = set()  # halfword indexes of the literal pool words its ldr rX, [pc, #imm] read: data, never a call
    pool_first = set()  # the index of each such word's first halfword
    for i, h in enumerate(hs):
        if h >> 11 == 0x09:
            t = ((start + 2 * i + 4) & ~3) + (h & 0xFF) * 4
            pool.update(((t - start) // 2, (t - start) // 2 + 1))
            pool_first.add((t - start) // 2)
    calls = []
    i = 0
    while i < len(hs):
        h = hs[i]
        if i in pool:
            i += 1
            continue
        if 0xF000 <= h < 0xF800 and i + 1 < len(hs) and hs[i + 1] >> 11 in (0x1F, 0x1D):
            h2 = hs[i + 1]
            off = start + 2 * i
            v = ((h & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
            t = off + 4 + (v - (1 << 23) if v & (1 << 22) else v)
            calls.append((i, resolve(at(t & ~3 if h2 >> 11 == 0x1D else t))))
            hs[i], hs[i + 1] = 0xF000, h2 & 0xF800
            i += 2
            continue
        i += 1
    # only the pool words (the alignment of a word differs between two copies of a function: Omega Ruby's 00304C8C and
    # 003054F6 are the same code at 4n and 4n + 2)
    for i in sorted(pool_first):
        if 0 <= i and i + 1 < len(hs):
            off = start + 2 * i
            w = hs[i] | (hs[i + 1] << 16)
            if is_address(off, w):
                hs[i] = hs[i + 1] = 0xADD2
    return tuple(hs), tuple(calls)


def calls_in(data, start, end, at, resolve):
    """(word index, called place) of a function's bl / blx imm, in order (at(offset) gives a branch target's place)"""
    out = []
    pool = set()  # the words its pc-relative loads read: data, never a call
    for off in range(start, end, 4):
        pool.update(function_list.pc_relative_words(struct.unpack_from("<I", data, off)[0], off))
    for off in range(start, end, 4):
        w = struct.unpack_from("<I", data, off)[0]
        if off in pool or function_list.text_word(data, off):
            continue  # (strings inside a function can read as a bl: Omega Ruby's 00136D04)
        if (w & 0x0E000000) == 0x0A000000 and ((w >> 24) & 1 or (w >> 28) == 0xF):
            imm = w & 0xFFFFFF
            t = off + 8 + ((imm - (1 << 24)) if imm & 0x800000 else imm) * 4
            if (w >> 28) == 0xF:
                t += ((w >> 24) & 1) * 2  # blx imm: its H bit
            out.append(((off - start) // 4, resolve(at(t))))
    return tuple(out)


def load(code_path, list_path, base, cro=None):
    """({place: masked words}, {place: called places}); a place is ("", address) in the .code, (module, file offset) in a
    module. A call to an import veneer is resolved to the function the import names, as the loader would."""
    data = open(code_path, "rb").read()
    rows = [line.rstrip("\n").split("\t") for line in open(list_path) if not line.startswith("#")]
    starts = sorted(int(r[0], 16) for r in rows)
    thumb = {("", int(r[0], 16)) for r in rows if r[3] == "thumb"}
    size = {("", int(r[0], 16)): int(r[4]) for r in rows}
    end = base + len(data)
    word = lambda d, o: struct.unpack_from("<I", d, o)[0] if 0 <= o <= len(d) - 4 else None
    veneer_of = {}  # a veneer's place -> the place it jumps to

    def resolve(k):
        return veneer_of.get(k, k)

    for s in starts:  # the .code's own veneers: ldr pc, [pc, #-4] and an absolute address
        if word(data, s - base) == 0xE51FF004:
            veneer_of[("", s)] = ("", word(data, s - base + 4))
    funcs, calls = {}, {}
    for i, s in enumerate(starts):
        e = min(s + (size[("", s)] or MAX_EXTENT), s + MAX_EXTENT, end)  # its own code (the list's size)
        if ("", s) not in thumb:
            funcs[("", s)] = masked(data, s - base, e - base, lambda off, w: base <= w < 0x01000000)
    if cro:
        cro_dir, cro_list = cro
        mods = {m.name: m for m in cro_functions.load(cro_dir)}
        static = cro_functions.Module(os.path.join(cro_dir, "static.crs"))  # segments at the .code's own addresses
        named = {}
        for m in mods.values():
            for n, t in m.named_exports().items():
                named[n] = (m.name, t)
        for n, t in static.named_exports().items():
            named[n] = ("", t)

        def target(imp):  # the imported function's place (bit 0, a Thumb mark, dropped)
            k = target_raw(imp)
            return (k[0], k[1] & ~1) if k else None

        def target_raw(imp):
            if imp[0] == "named":
                return named.get(imp[1])
            module = imp[1]
            if module == "|static|":
                t = static.tag(imp[2]) if imp[0] == "anon" else static.indexed_export(imp[2])
                return ("", t) if t is not None else None
            m = mods.get(module)
            if m is None:
                return None
            t = m.tag(imp[2]) if imp[0] == "anon" else m.indexed_export(imp[2])
            return (module, t) if t is not None else None

        by_mod = collections.defaultdict(list)
        for line in open(cro_list):
            if not line.startswith("#"):
                name, off, _, isa, n = line.rstrip("\n").split("\t")
                by_mod[name].append(int(off, 16))
                size[(name, int(off, 16))] = int(n)
                if isa == "thumb":
                    thumb.add((name, int(off, 16)))
        for name, offs in by_mod.items():
            m = mods[name]
            imports = m.imports()
            for s in offs:
                if word(m.data, s) == 0xE51FF004 and s + 4 in imports:
                    veneer_of[(name, s)] = target(imports[s + 4]) or ("?", s)
            offs.sort()
            for i, s in enumerate(offs):
                e = min(s + (size[(name, s)] or MAX_EXTENT), s + MAX_EXTENT, m.code_end)
                if (name, s) in thumb:
                    funcs[(name, s)], calls[(name, s)] = thumb_scan(
                        m.data, s, e, lambda off, w, r=m.relocated: off in r, lambda t, n=name: (n, t), resolve)
                    continue
                funcs[(name, s)] = masked(m.data, s, e, lambda off, w, r=m.relocated: off in r)
                calls[(name, s)] = calls_in(m.data, s, e, lambda t, n=name: (n, t), resolve)
    for i, s in enumerate(starts):
        e = min(s + (size[("", s)] or MAX_EXTENT), s + MAX_EXTENT, end)
        if ("", s) in thumb:
            funcs[("", s)], calls[("", s)] = thumb_scan(data, s - base, e - base, lambda off, w: base <= w < 0x01000000,
                                                        lambda t: ("", t + base), resolve)
        else:
            calls[("", s)] = calls_in(data, s - base, e - base, lambda t: ("", t + base), resolve)
    return sorted(funcs), funcs, calls


def place(k):
    return "%08X" % k[1] if not k[0] else "%s+%06X" % k


def digest(words):
    return hashlib.blake2b(struct.pack("<%dI" % len(words), *words), digest_size=16).digest()


SLOTS = {}  # (oras place, x place) -> call_slots: they depend on the two functions only


def pair_slots(oa, xa, o, x, o_calls, x_calls):
    k = (oa, xa)
    if k not in SLOTS:
        SLOTS[k] = call_slots(o[oa], x[xa], o_calls.get(oa, ()), x_calls.get(xa, ()))
    return SLOTS[k]


def call_slots(ow, xw, oc, xc):
    """the calls at the same place of the two functions' aligned words: [(oras target, x target)]"""
    if ow == xw:
        return [(t, u) for (_, t), (_, u) in zip(oc, xc)]
    xi = dict(xc)
    out = []
    for blk in difflib.SequenceMatcher(None, ow, xw, autojunk=False).get_matching_blocks():
        for i, t in oc:
            if blk.a <= i < blk.a + blk.size and (i - blk.a + blk.b) in xi:
                out.append((t, xi[i - blk.a + blk.b]))
    return out


def propagate(pairs, o, x, o_calls, x_calls, todo):
    """Pairs by calls: every pair in todo proposes, for each place its two functions call, the X target for the Omega Ruby
    one; a target neither paired yet takes the X function most of its callers propose (a tie takes none), "repeated"
    when the bodies are equal, else "call" when at least CALL_MIN alike; its own calls are followed in the next round,
    until no new pair."""
    todo = list(todo)
    while todo:
        paired_x = {v[0] for v in pairs.values()}
        votes = collections.defaultdict(collections.Counter)
        for oa in todo:
            xa = pairs[oa][0]
            for t, u in pair_slots(oa, xa, o, x, o_calls, x_calls):
                if t not in pairs and u not in paired_x and t in o and u in x:
                    votes[t][u] += 1
        new = []
        for t, c in sorted(votes.items()):
            (u, n), *rest = c.most_common(2)
            if rest and rest[0][1] == n or u in paired_x:
                continue
            if o[t] == x[u]:  # one of several copies of a same content, told apart by its callers
                pairs[t] = (u, "repeated", 1.0)
            else:
                sm = difflib.SequenceMatcher(None, o[t], x[u], autojunk=False)
                r = sm.ratio() if len(o[t]) * len(x[u]) < 4_000_000 else sm.quick_ratio()
                if r < CALL_MIN:
                    continue
                pairs[t] = (u, "call", r)
            paired_x.add(u)
            new.append(t)
        todo = new


def check_calls(pairs, o, x, o_calls, x_calls):
    """Every pair's calls, slot by slot, once all pairs are made: the Omega Ruby target's X copy must be the X target.
    Returns {pair: (agreeing, contradicting, with an unpaired target)}: a slot contradicts when either target is paired
    to another function; a pair with a contradicting slot gets the kind "<kind>-body" (same body, other calls)."""
    paired_x = {v[0] for v in pairs.values()}
    result = {}
    for oa, (xa, kind, r) in list(pairs.items()):
        ok = bad = unpaired = 0
        for t, u in pair_slots(oa, xa, o, x, o_calls, x_calls):
            if t in pairs and pairs[t][0] == u:
                ok += 1
            elif t in pairs or u in paired_x:
                bad += 1
            else:
                unpaired += 1
        result[oa] = (ok, bad, unpaired)
        if bad:
            pairs[oa] = (xa, kind + "-body", r)
    return result


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
    o_starts, o, o_calls = load(a.oras_code, a.oras_list, a.base, a.cro and (a.cro[0], a.cro[2]))
    x_starts, x, x_calls = load(a.x_code, a.x_list, a.base, a.cro and (a.cro[1], a.cro[3]))

    o_by = collections.defaultdict(list)
    x_by = collections.defaultdict(list)
    # a module's import veneers (ldr pc, [pc, #-4] and the imported address: 18,923 in Omega Ruby's modules) are not
    # functions of the game: their pair follows from the import's name, not from their two words
    veneer = lambda w: w[:1] == (0xE51FF004,) and len(w) <= 2  # ldr pc, [pc, #-4] (its address word after it)
    o = {k: w for k, w in o.items() if not veneer(w)}
    x = {k: w for k, w in x.items() if not veneer(w)}
    o_starts = [k for k in o_starts if k in o]
    x_starts = [k for k in x_starts if k in x]
    for s, w in o.items():
        o_by[digest(w)].append(s)
    for s, w in x.items():
        x_by[digest(w)].append(s)
    pairs = {}
    groups = []  # the same content several times in a game: paired by their callers (check_calls), not by address
    for h, os_ in o_by.items():
        xs = x_by.get(h)
        if not xs:
            continue
        if len(os_) == 1 and len(xs) == 1:
            pairs[os_[0]] = (xs[0], "identical", 1.0)
        else:
            groups.append((os_, xs))

    # calls first: the copies of a repeated content and the changed functions their identical callers name
    propagate(pairs, o, x, o_calls, x_calls, list(pairs))
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
    propagate(pairs, o, x, o_calls, x_calls, list(prefix) + list(near))
    # a group of copies left with one unpaired copy in each game: that pair is the only one possible
    paired_x = {v[0] for v in pairs.values()}
    forced = []
    for os_, xs in groups:
        ol = [k for k in os_ if k not in pairs]
        xl = [k for k in xs if k not in paired_x]
        if len(ol) == 1 and len(xl) == 1:
            pairs[ol[0]] = (xl[0], "repeated", 1.0)
            paired_x.add(xl[0])
            forced.append(ol[0])
    if forced:
        propagate(pairs, o, x, o_calls, x_calls, forced)
    # a pair resting on likeness or on one call (call, near, prefix) whose own calls contradict the other pairs is not
    # proven: removed, and the calls followed again without it, until none is left
    weak = ("call", "near", "prefix")
    while True:
        slots = check_calls(pairs, o, x, o_calls, x_calls)
        drop = [k for k, v in slots.items() if v[1] and pairs[k][1].replace("-body", "") in weak]
        for k in pairs:
            pairs[k] = (pairs[k][0], pairs[k][1].replace("-body", ""), pairs[k][2])
        if not drop:
            slots = check_calls(pairs, o, x, o_calls, x_calls)
            break
        for k in drop:
            del pairs[k]
        propagate(pairs, o, x, o_calls, x_calls, list(pairs))
    ambiguous_o = sum(1 for os_, _ in groups for k in os_ if k not in pairs)
    print("calls checked: %d agree, %d contradict, %d with a target in no pair; %d pairs found through calls; pairs with a "
          "contradicting call: %d" % (sum(v[0] for v in slots.values()), sum(v[1] for v in slots.values()),
                                       sum(v[2] for v in slots.values()),
                                       sum(1 for v in pairs.values() if v[1].startswith("call")),
                                       sum(1 for v in slots.values() if v[1])))

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
