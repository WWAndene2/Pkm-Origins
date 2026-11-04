#!/usr/bin/env python3
"""The system calls and service requests a running game made (docs/RECOMPILATION_PLAN.md §10, step 5: what `src/hos/`
must answer, the most frequent first), from the counts of tools/azahar_trace_run.sh (every call counted: no record
limit), Azahar's names.

  hos_calls.py <counts.txt> [<counts.txt>...]    (several: a column each, e.g. both games)

Prints the kernel calls by id, then the service requests by service, each request with its IPC headers. Writes nothing.
"""
import argparse
import collections


def main():
    p = argparse.ArgumentParser()
    p.add_argument("counts", nargs="+")
    a = p.parse_args()
    runs = []
    for path in a.counts:
        c, total = collections.Counter(), 0
        for line in open(path):
            if line.startswith("# calls "):
                total = int(line.split()[2])
            elif line.strip():
                key, n = line.rstrip("\n").rsplit(" ", 1)
                c[key] = int(n)
        runs.append(c)
        print("%s: %d calls, %d distinct (call, header)" % (path, total, len(c)))
    keys = set().union(*runs)
    svc = sorted((k for k in keys if k.startswith("svc ")), key=lambda k: int(k.split()[1], 16))
    print("kernel calls:" + "".join("  %10s" % ("run %d" % (i + 1)) for i in range(len(runs))))
    for k in svc:
        print("  %-40s%s" % (k[4:], "".join("  %10d" % r[k] for r in runs)))
    by_request = collections.defaultdict(list)
    for k in keys:
        if k.startswith("ipc "):
            name, header = k[4:].rsplit(" ", 1)
            by_request[name].append((header, k))
    print("service requests (service:request, then each header):")
    for name in sorted(by_request, key=lambda n: (n.split(":")[0], n)):
        hs = sorted(by_request[name])
        print("  %-52s%s" % (name, "".join("  %10d" % sum(r[k] for _, k in hs) for r in runs)))
        if len(hs) > 1:
            for h, k in hs:
                print("      %s%s" % (h.ljust(46), "".join("  %10d" % r[k] for r in runs)))
    print("%d kernel calls, %d service requests" % (len(svc), len(by_request)))


if __name__ == "__main__":
    main()
