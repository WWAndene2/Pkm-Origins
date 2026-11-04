#!/usr/bin/env python3
"""The translated game's system calls against Azahar's, thread by thread (docs/RECOMPILATION_PLAN.md §10, step 3):
where the two first differ is where src/hos/ answered something else, or the game took another path.

  hos_trace_compare.py <Azahar's calls.txt> <ours.log> [thread...]

Azahar's: a record of Pomegrade's code trace (calls.txt: every call with its thread, pc, lr and first words; a record
that kept them all, as tools/azahar_trace_run.sh's first records do). Ours: the game run with HOS_TRACE=1, its stderr.
Compared per thread (threads interleave differently in the two), by call (svc number, or service:command), lr, and an
IPC request's header; handle values and the rest of the words differ legitimately (each numbers its handles its own
way) and are printed, not compared. Prints, for each thread, how many calls agree and the first that does not.
"""
import argparse
import collections
import re


def azahar(path):
    out = collections.defaultdict(list)
    for line in open(path):
        m = re.search(r"thread (\d+) pc \S+ lr (\S+) (svc|ipc) (.*)$", line.rstrip())
        if m:
            f = m.group(4).split()
            out[int(m.group(1))].append((m.group(3), f[0], m.group(2), f[2:] if m.group(3) == "svc" else f[1:]))
    return out


def ours(path):
    out = collections.defaultdict(list)
    for line in open(path):
        line = line.replace("(fs file):", ":")
        m = re.match(r"trace thread (\d+) lr (\S+) svc (\S+) (.*)", line)
        if m:
            out[int(m.group(1))].append(("svc", m.group(3), m.group(2), m.group(4).split()))
            continue
        m = re.match(r"trace thread (\d+) ipc (\S+) (.*)", line)
        if m:
            out[int(m.group(1))].append(("ipc", m.group(2), None, m.group(3).split()))
    return out


def same(x, y):
    return x[0] == y[0] and x[1] == y[1] and (not x[2] or not y[2] or x[2] == y[2]) and \
        (x[0] != "ipc" or x[3][:1] == y[3][:1])


def main():
    p = argparse.ArgumentParser()
    p.add_argument("azahar")
    p.add_argument("ours")
    p.add_argument("threads", nargs="*", type=int)
    a = p.parse_args()
    az, us = azahar(a.azahar), ours(a.ours)
    for t in a.threads or sorted(set(us)):
        x, y = az.get(t, []), us.get(t, [])
        n = 0
        while n < min(len(x), len(y)) and same(x[n], y[n]):
            n += 1
        print("thread %d: %d calls agree (Azahar's record %d, ours %d)" % (t, n, len(x), len(y)))
        if n < min(len(x), len(y)):
            for k in range(max(0, n - 3), min(n + 3, len(x), len(y))):
                print("  %s %4d  Azahar %-36s lr %s %s" % ("->" if k == n else "  ", k, x[k][0] + " " + x[k][1],
                                                           x[k][2], " ".join(x[k][3][:4])))
                print("          ours   %-36s lr %s %s" % (y[k][0] + " " + y[k][1], y[k][2], " ".join(y[k][3][:4])))


if __name__ == "__main__":
    main()
