#!/usr/bin/env python3
"""The system calls and service requests a running game made (docs/RECOMPILATION_PLAN.md §10, step 5: what `src/hos/`
must answer, in the order it matters), from the record of Pomegrade's code trace (tools/azahar_trace_preload.c).

  hos_calls.py <record folder or snapshots folder>...

Prints each kernel call and each service request (service:request, as Azahar names them) with how many times it came
and from which threads, the most frequent first; and whether every call is counted (a record's ring holds the last
131,072: tools/azahar_trace_run.sh copies each record written, and the report says whether they overlap with no gap).
Writes nothing.
"""
import argparse
import collections
import os
import re


def main():
    p = argparse.ArgumentParser()
    p.add_argument("records", nargs="+", help="record folders (calls.txt, summary.txt), or a folder of them "
                   "(tools/azahar_trace_run.sh's snapshots/), joined in name order")
    a = p.parse_args()
    folders = []
    for r in a.records:
        if os.path.exists(os.path.join(r, "calls.txt")):
            folders.append(r)
        else:
            folders += sorted(os.path.join(r, d) for d in os.listdir(r) if os.path.exists(os.path.join(r, d, "calls.txt")))
    svc, ipc, threads = collections.Counter(), collections.Counter(), collections.defaultdict(set)
    calls, prev, gaps, total = [], [], 0, 0
    for r in folders:
        m = re.search(r"service requests: (\d+), the last (\d+) kept", open(os.path.join(r, "summary.txt")).read())
        total, kept = int(m.group(1)), int(m.group(2))
        cur = [l.rstrip("\n") for l in open(os.path.join(r, "calls.txt")) if " thread " in l]
        # the record's calls after the previous record's last: the previous record's tail is where this one starts
        # (aligned on the whole overlap, not on single lines: identical calls in a row are kept as many as they are)
        new = None
        if not prev:
            new = cur if total == kept else None
        else:
            for j in [k for k, line in enumerate(prev) if line == cur[0]] if cur else []:
                if prev[j:] == cur[:len(prev) - j]:
                    new = cur[len(prev) - j:]
                    break
            if new is None and total == kept and cur[:len(prev)] == prev:
                new = cur[len(prev):]
        if new is None:
            gaps += 1
            print("%s: does not continue the record before it (calls lost in between)" % r)
            new = cur
        calls += new
        prev = cur
    for line in calls:
        m = re.search(r"thread (\d+) pc \S+ lr \S+ (svc|ipc) (.*?)((?: [0-9A-F]{8})*)$", line)
        if m:
            name = m.group(3)
            (svc if m.group(2) == "svc" else ipc)[name] += 1
            threads[name].add(int(m.group(1)))
    print("%d records joined: %d calls of the %d the game made%s" % (
        len(folders), len(calls), total, ", none lost" if not gaps and len(calls) == total else
        ", %d gaps: some calls are not counted" % gaps))
    print("kernel calls:")
    for name, n in svc.most_common():
        print("  %8d  %-32s threads %s" % (n, name, ",".join(map(str, sorted(threads[name])))))
    print("service requests:")
    for name, n in ipc.most_common():
        print("  %8d  %-48s threads %s" % (n, name, ",".join(map(str, sorted(threads[name])))))
    print("%d kernel calls and %d service requests, distinct" % (len(svc), len(ipc)))


if __name__ == "__main__":
    main()
