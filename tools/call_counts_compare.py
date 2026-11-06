#!/usr/bin/env python3
"""Azahar's counts of the game's calls against ours (docs/RECOMPILATION_PLAN.md §10, step 3): the calls one makes and
the other does not, then those whose counts differ most. A request only Azahar's run makes is one the game did not
reach with us, or took another path for; one only ours makes, a path Azahar's never took.

  call_counts_compare.py <Azahar's counts.txt> <ours counts.txt> [--ratio 4]

Azahar's: tools/azahar_trace_run.sh's counts.txt ("svc <id> <name> <n>", "ipc <service>:<request> <header> <n>").
Ours: tools/hos_call_counts.sh's ("svc <id> <n>", "ipc <service>:<request> <n>"). Kernel calls compared by id, service
requests by service and Azahar's request name (ours use the same names). The two runs need not be as long: the counts
are printed as they are, for the reader to weigh (a run twice as long makes twice the per-frame calls).
"""
import argparse


def azahar(path):
    c = {}
    for line in open(path):
        f = line.split()
        if not f or f[0].startswith("#"):
            continue
        if f[0] == "svc":
            c["svc %02X" % int(f[1], 16)] = c.get("svc %02X" % int(f[1], 16), 0) + int(f[-1])
            names["svc %02X" % int(f[1], 16)] = f[2]
        elif f[0] == "ipc":
            c["ipc " + f[1]] = c.get("ipc " + f[1], 0) + int(f[-1])
    return c


def ours(path):
    c = {}
    for line in open(path):
        f = line.split()
        if len(f) < 3:
            continue
        key = "svc %02X" % int(f[1], 16) if f[0] == "svc" else "ipc " + f[1]
        c[key] = c.get(key, 0) + int(f[-1])
    return c


names = {}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("azahar")
    p.add_argument("ours")
    p.add_argument("--ratio", type=float, default=4.0, help="a count this many times the other's is shown")
    a = p.parse_args()
    az, us = azahar(a.azahar), ours(a.ours)
    label = lambda k: k + (" " + names[k] if k in names else "")
    only_az = sorted((k for k in az if k not in us), key=lambda k: -az[k])
    only_us = sorted((k for k in us if k not in az), key=lambda k: -us[k])
    print("only in Azahar's run (%d):" % len(only_az))
    for k in only_az:
        print("  %8d  %s" % (az[k], label(k)))
    print("only in ours (%d):" % len(only_us))
    for k in only_us:
        print("  %8d  %s" % (us[k], label(k)))
    print("in both, one count over %g times the other (Azahar, ours):" % a.ratio)
    for k in sorted(set(az) & set(us), key=lambda k: -max(az[k], us[k]) / max(1, min(az[k], us[k]))):
        if max(az[k], us[k]) >= a.ratio * max(1, min(az[k], us[k])):
            print("  %8d %8d  %s" % (az[k], us[k], label(k)))


if __name__ == "__main__":
    main()
