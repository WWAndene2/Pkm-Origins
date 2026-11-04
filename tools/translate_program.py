#!/usr/bin/env python3
"""The translation of a whole program or module (tools/arm2c.py on every function of a function list, on --jobs
processes), shared by the matching check (tools/match_functions.py) and the game's build (tools/build_game.py).

Besides the listed functions it translates the entries other functions branch into (code the compiler shares between
functions: from the branch's target to the end of the function holding it, or to the next start when the target is
past the function list's end of the one before), until none is new.
"""
import bisect
import multiprocessing
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arm2c  # noqa: E402

TRANSLATE = None  # (code, base, sizes, ARM starts, Thumb starts, relocated words, module), set before the processes fork


def translate_one(s, end=None, is_thumb=None):
    """(s, ((C, size), callees, {entry: its instruction set})), or (s, the reason it is not translated). end and
    is_thumb: an entry inside a function (a branch's target there), translated to that function's end in the
    instruction set of the branch that reaches it (a b never changes it: Omega Ruby's ARM veneer at 00100184 sits in
    the range of the Thumb function before it)"""
    code, base, size, arm, thumb, relocated, module = TRANSLATE
    try:
        callees, entries = set(), set()
        isa = s in thumb if is_thumb is None else is_thumb
        c = arm2c.translate(code, base, s, end or s + min(size[s] or 0x400, arm2c.MAX_FUNCTION), arm2c.fname(s, module), arm,
                            callees, relocated, isa, thumb, entries, module)
        return s, (c, callees, {e: isa for e in entries})
    except arm2c.Unsupported as x:
        return s, "instruction " + str(x).split()[1] if str(x).startswith("instruction") else str(x)



def translate_program(code, base, starts, size, thumb, relocated, module=None, jobs=1):
    """({start: (C, size)}, {start: callees}, {start: why refused}, {reason: count}, the Thumb entries): every
    function of the list (starts, their sizes, the Thumb ones) and the entries reached, translated"""
    global TRANSLATE
    arm = frozenset(x for x in starts if x not in thumb)
    TRANSLATE = (code, base, size, arm, frozenset(thumb), relocated, module)
    funcs, reasons, deps = {}, {}, {}
    with multiprocessing.get_context("fork").Pool(jobs) as pool_:
        done = pool_.map(translate_one, starts, chunksize=256)
    why = {}  # each refused function's own reason
    entry_isa = {}  # each entry's instruction set: that of the branches reaching it
    for s, got in done:
        if isinstance(got, str):
            reasons[got] = reasons.get(got, 0) + 1
            why[s] = got
        else:
            (c, n), callees, ents = got
            funcs[s], deps[s] = (c, n), callees
            for e, isa in ents.items():
                entry_isa.setdefault(e, isa)
    # the entries inside functions their branches reach (shared tails), translated as functions of their own, from
    # there to the end of the function holding them, until none is new (an entry's code may branch into another)
    entry_thumb = set()
    while True:
        new = {t for d in deps.values() for t in d if t not in funcs and t not in why and t not in size}
        if not new:
            break
        for t in sorted(new):
            k = bisect.bisect(starts, t) - 1
            f = starts[k] if k >= 0 else None
            if f is None:
                why[t] = "branch to no function's code"
                reasons[why[t]] = reasons.get(why[t], 0) + 1
                continue
            # past the end of the function before it (code the function list's walk did not reach): to the next start
            nxt = starts[k + 1] if k + 1 < len(starts) else f + (size[f] or 0)
            # (to the next start: the function holding the entry ends there at the latest; its own size, as the walk
            # measured it, can stop short of code only branches reach, as the veneer's bx ip at 00100188)
            code_end = base + len(code)
            if t >= code_end or nxt <= t:  # (past the last function: nothing listed holds it)
                why[t] = "branch to no function's code"
                reasons[why[t]] = reasons.get(why[t], 0) + 1
                continue
            end = min(nxt, t + arm2c.MAX_FUNCTION, code_end)
            isa = entry_isa.get(t, f in thumb)
            _, got = translate_one(t, end, isa)
            if isinstance(got, str):
                why[t] = got
                reasons["(entry) " + got] = reasons.get("(entry) " + got, 0) + 1
            else:
                (c, n), callees, ents = got
                funcs[t], deps[t] = (c, n), callees
                for e, eisa in ents.items():
                    entry_isa.setdefault(e, eisa)
                if isa:
                    entry_thumb.add(t)

    return funcs, deps, why, reasons, entry_thumb
