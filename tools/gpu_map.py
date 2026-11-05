#!/usr/bin/env python3
"""The game's graphics library as it runs (step 4 of docs/RECOMPILATION_PLAN.md §3): from a GPU trace run
(build_game.py --gpu-trace; $HOS_GPU_TRACE and $HOS_GPU_TRACE_CALLS, src/hos/GpuTrace.c), the functions that build the
GPU's command lists and those above them, and where the rest of the game calls into them.

  gpu_map.py <gpu trace.tsv> <calls.tsv> [--names function_names.tsv] [--depth 3] [--seconds 60] [--out map.tsv]
             [--copies ADDR,ADDR...]

--copies: copy helpers (memcpy, fixed-size copies): a word they write is credited to the first function of the
trace's chain above them that is not one, the author of what is copied (the chain holds 4 functions: a word whose
chain is copies only stays with the last).

- writers: the functions the trace credits with command list words (written, or copied from the program's data: the
  copy's caller), with their share of the words;
- the library: the writers and the functions that call them, up to --depth calls above (the call graph as run);
- its entries: the library's functions called from outside it, with the callers outside and the calls a second.

The output holds addresses, counts and names only (no game bytes): it may be committed. --names: Pomegrade's
tools/remake/ghidra/function_names.tsv for Omega Ruby (read where it is), or for X a file of X addresses and names.
"""
import argparse
import collections


def main():
    p = argparse.ArgumentParser()
    p.add_argument("trace")
    p.add_argument("calls")
    p.add_argument("--names")
    p.add_argument("--depth", type=int, default=3)
    p.add_argument("--seconds", type=float, default=60)
    p.add_argument("--out")
    p.add_argument("--copies", default="")
    a = p.parse_args()
    names = {}
    if a.names:
        for line in open(a.names):
            if not line.startswith("#"):
                f = line.rstrip("\n").split("\t")
                if len(f) >= 2:
                    names[int(f[0], 16)] = f[1]
    copies = {int(c, 16) for c in a.copies.split(",") if c}

    def author(f):
        chain = [int(x, 16) for x in f[1:5] if x != "-"]
        for fn in chain:
            if fn not in copies:
                return fn
        return chain[-1]
    share, fresh = collections.Counter(), collections.Counter()
    total = 0
    for line in open(a.trace):
        if line.startswith("#"):
            continue
        f = line.rstrip("\n").split("\t")
        if f[0] in ("write", "copy"):
            share[author(f)] += int(f[5])
            total += int(f[5])
            if len(f) > 8 and f[8] != "-":
                fresh[author(f)] += int(f[8])
        else:
            total += int(f[5])
    callers, callees = collections.defaultdict(collections.Counter), collections.defaultdict(collections.Counter)
    calls = collections.Counter()
    for line in open(a.calls):
        if line.startswith("#"):
            continue
        fr, to, n = line.split("\t")
        fr, to, n = int(fr, 16), int(to, 16), int(n)
        callers[to][fr] += n
        callees[fr][to] += n
        calls[to] += n
    level = {w: 0 for w in share}
    frontier = set(share)
    for d in range(1, a.depth + 1):
        nxt = set()
        for f in frontier:
            for c in callers[f]:
                if c and c not in level:
                    level[c] = d
                    nxt.add(c)
        frontier = nxt
    library = set(level)
    entries = {f: {c: n for c, n in callers[f].items() if c not in library} for f in library}
    entries = {f: cs for f, cs in entries.items() if cs}
    rate = lambda n: n / a.seconds
    print("writers: %d (%d words); the library to %d calls above: %d functions; entries from outside: %d"
          % (len(share), total, a.depth, len(library), len(entries)))
    print("words written in the last 32 lists (built each frame): %.1f%%; the rest built earlier and submitted again"
          % (100 * sum(fresh.values()) / total))
    print("the writers by their share of the words (and of those, built each frame):")
    for f, n in share.most_common(15):
        print("  %08X %5.1f%% (%5.1f%% each frame) %8.0f calls/s  %s" % (f, 100 * n / total, 100 * fresh[f] / total,
                                                                         rate(calls[f]), names.get(f, "")))
    print("the entries by calls a second (the callers outside the library):")
    for f in sorted(entries, key=lambda f: -sum(entries[f].values()))[:25]:
        out = ", ".join("%08X" % c for c, _ in sorted(entries[f].items(), key=lambda kv: -kv[1])[:3])
        print("  %08X level %d %8.0f calls/s  %-36s from %s" % (f, level[f], rate(sum(entries[f].values())),
                                                                  names.get(f, ""), out))
    if a.out:
        with open(a.out, "w") as o:
            o.write("# address\tlevel (0: writes command words)\tshare of the words %\tof them built each frame %"
                    "\tcalls a second\tentry (called from outside)\tname (Pomegrade)\n")
            for f in sorted(library, key=lambda f: (level[f], f)):
                o.write("%08X\t%d\t%.2f\t%.2f\t%.0f\t%s\t%s\n" % (
                    f, level[f], 100 * share[f] / total if total else 0, 100 * fresh[f] / total if total else 0,
                    rate(calls[f]), "yes" if f in entries else "", names.get(f, "")))


if __name__ == "__main__":
    main()
