#!/usr/bin/env python3
"""Where a running program spends its time, by sampling its threads' stacks with gdb (no perf in the session's
container): each sample, every thread's innermost functions of ours (C library and driver frames set aside), counted.
6 November: it showed the renderer's speed went to the CPU vertex shaders, the frames written to disk and the GPU trace
of the test build, not to the submissions first suspected.

  sample_stacks.py <pid> [--samples 20] [--interval 0.5] [--depth 3]

Prints each innermost call chain (up to --depth functions) with the number of thread samples in it; "(waiting)" counts
the threads blocked in the C library or the driver. A thread holding the game's CPU shows in game or renderer
functions; the others wait in hos_block.
"""
import argparse
import collections
import re
import subprocess
import time

LIBRARY = ("__", "_IO", "futex", "pthread", "clone", "start_thread", "syscall")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("pid", type=int)
    p.add_argument("--samples", type=int, default=20)
    p.add_argument("--interval", type=float, default=0.5)
    p.add_argument("--depth", type=int, default=3)
    a = p.parse_args()
    counts = collections.Counter()
    for _ in range(a.samples):
        out = subprocess.run(["gdb", "-p", str(a.pid), "-batch", "-ex", "thread apply all bt 16"],
                             capture_output=True, text=True).stdout
        for thread in out.split("\nThread ")[1:]:
            ours = [f for f in re.findall(r" in ([A-Za-z_0-9]+) \(", thread) if not f.startswith(LIBRARY)]
            counts[" < ".join(ours[:a.depth]) if ours else "(waiting)"] += 1
        time.sleep(a.interval)
    for chain, n in counts.most_common(30):
        print("%5d  %s" % (n, chain))


if __name__ == "__main__":
    main()
