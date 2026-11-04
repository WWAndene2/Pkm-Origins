#!/usr/bin/env python3
"""Why tools/arm2c.py refuses the functions it refuses, down to the instruction: each reason with how many functions
it stops, then for the reasons asked the instructions it stopped on (mnemonic and operands) with an example
address, the most frequent first. What to translate next, and how, is read here.

  refusals.py <code.bin> <functions.tsv> [--cro DIR] [--reason TEXT ...] [--forms N]

Instructions with no C do not refuse a function: each is a stop (fault 10); they are listed apart, by reason (and
their instructions for the reasons asked). Each function is translated alone (callees not followed: tools/match_functions.py says which wait for which). The
instruction is the one being translated when the refusal was raised (from the translator's own frame).
"""
import argparse
import collections
import re
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arm2c  # noqa: E402


def stopped_on(tb):
    """the instruction the translator was on when it refused: its innermost frame's i (or md_insn)"""
    insn = None
    while tb:
        for name in ("i", "md_insn"):
            v = tb.tb_frame.f_locals.get(name)
            if v is not None and hasattr(v, "mnemonic"):
                insn = v
        tb = tb.tb_next
    return insn


def main():
    p = argparse.ArgumentParser()
    p.add_argument("code")
    p.add_argument("functions")
    p.add_argument("--cro")
    p.add_argument("--base", type=lambda s: int(s, 0), default=0x100000)
    p.add_argument("--reason", nargs="*", default=[], help="reasons whose instructions to list (a part of the text)")
    p.add_argument("--forms", type=int, default=10)
    a = p.parse_args()
    code = open(a.code, "rb").read()
    rows = [l.split() for l in open(a.functions) if l.strip() and not l.startswith("#")]
    thumb = frozenset(int(r[0], 16) for r in rows if r[3] == "thumb")
    arm = frozenset(int(r[0], 16) for r in rows if r[3] != "thumb")
    size = {int(r[0], 16): int(r[4]) for r in rows}
    relocated = arm2c.static_relocations(a.cro) if a.cro else frozenset()
    reasons, forms, example = collections.Counter(), collections.defaultdict(collections.Counter), {}
    stops = collections.Counter()
    for s in sorted(size):
        try:
            c, _ = arm2c.translate(code, a.base, s, s + min(size[s] or 0x400, arm2c.MAX_FUNCTION), "f", arm, None, relocated,
                                   s in thumb, thumb, set())
            # the instructions it has no C for: a stop each (fault 10), the function translated
            for m in re.finditer(r"/\* ([0-9A-F]{8}) ([^*]*?) \*/[^\n]*UNSUPPORTED: ([^*]*?) \*/", c):
                why = "stop: " + m.group(3)
                stops[why] += 1
                forms[why][m.group(2)] += 1
                example.setdefault((why, m.group(2)), "%s in %08X" % (m.group(1), s))
            for m in re.finditer(r"no return past here", c):
                stops["stop: no return found"] += 1
        except arm2c.Unsupported as e:
            why = str(e)
            reasons[why] += 1
            if any(r in why for r in a.reason):
                insn = stopped_on(sys.exc_info()[2])
                form = "%s %s" % (insn.mnemonic, insn.op_str) if insn is not None else "?"
                forms[why][form] += 1
                example.setdefault((why, form), "%08X in %08X" % (insn.address, s) if insn is not None else "%08X" % s)
    print("%d functions, %d refused alone:" % (len(size), sum(reasons.values())))
    for why, n in reasons.most_common():
        print("  %6d  %s" % (n, why))
        for form, k in forms[why].most_common(a.forms):
            print("          %5d  %-44s e.g. %s" % (k, form, example[(why, form)]))
    print("%d instructions translated as a stop (fault 10: no C for them; data the walk took for code, mostly):"
          % sum(stops.values()))
    for why, n in stops.most_common():
        print("  %6d  %s" % (n, why))
        if any(r in why for r in a.reason):
            for form, k in forms[why].most_common(a.forms):
                print("          %5d  %-44s e.g. %s" % (k, form, example[(why, form)]))


if __name__ == "__main__":
    main()
