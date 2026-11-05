#!/usr/bin/env python3
"""The game's build (docs/RECOMPILATION_PLAN.md §10, step 2): the whole .code translated to C (tools/arm2c.py on every
function of the list and the entries reached, tools/translate_program.py) and compiled into a static library, on the
player's machine: the C holds the game's own constants, so it is written outside the repository and never committed
(CLAUDE.md §1).

  build_game.py <code.bin> <functions.tsv> <out dir> [--cro DIR] [--modules cro_functions.tsv] [--jobs N] [--cc cc]
               [--opt -O2] [--program] [--gpu-trace]

--modules: the CRO modules translated too (symbols/<game>_cro_functions.tsv, with --cro): their C writes addresses as
B_<module> + offset, set when ldr:ro loads the module (hos_module_loaded, in table.c) and cleared when it unloads it;
call_indirect then finds a module's functions by address while it is loaded.

Every function either translates or becomes a stub that stops its thread with fault 9 (HOS_FAULT_NOT_TRANSLATED, its
address in r15): nothing is guessed, and the report lists the stubs and why each one is. call_indirect finds any of
them by address (a Thumb one by its odd address). The library's only undefined symbols must be the runtime's
(src/hos/: hos_memory, hos_svc) and the C library's: checked after the build (nm), the report says so.

out/
  functions.h          a prototype per function
  part<k>.c            the functions, spread over the jobs
  table.c              call_indirect and the stubs
  libgame_code.a       the library
  build.txt            the report
"""
import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arm2c  # noqa: E402
import translate_program  # noqa: E402

HOS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "hos")
RUNTIME_SYMBOLS = {"hos_memory", "hos_svc"}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("code")
    p.add_argument("functions")
    p.add_argument("out")
    p.add_argument("--cro", help="the game's modules: the words they rewrite in the .code are read when run")
    p.add_argument("--base", type=lambda s: int(s, 0), default=0x100000)
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    p.add_argument("--cc", default="cc")
    p.add_argument("--opt", default="-O2")
    p.add_argument("--modules", help="the modules' function list (symbols/<game>_cro_functions.tsv; needs --cro)")
    p.add_argument("--gpu-trace", action="store_true", help="build the GPU trace in (src/hos/GpuTrace.h): slower")
    p.add_argument("--program", action="store_true", help="also link the runtime (src/hos/, src/platform/) into "
                   "<out>/game, the game's program (run: game <extracted dir> <the game's .3ds>)")
    a = p.parse_args()
    code = open(a.code, "rb").read()
    rows = [l.split() for l in open(a.functions) if l.strip() and not l.startswith("#")]
    starts = sorted(int(r[0], 16) for r in rows)
    thumb = {int(r[0], 16) for r in rows if r[3] == "thumb"}
    size = {int(r[0], 16): int(r[4]) for r in rows}
    relocated = arm2c.static_relocations(a.cro) if a.cro else frozenset()
    funcs, deps, why, reasons, entry_thumb = translate_program.translate_program(
        code, a.base, starts, size, thumb, relocated, None, a.jobs)
    thumb |= entry_thumb
    # the stubs: the listed functions not translated, and any callee not translated
    wanted = set(starts) | {t for d in deps.values() for t in d}
    stubs = sorted(t for t in wanted if t not in funcs)
    every = sorted(set(funcs) | set(stubs))
    # the modules: each translated as its file (offsets from 0), its functions named f_<module>_<offset>
    modules = []  # (name, size, {offset: C}, stubs, keys: sorted (offset with the Thumb bit, offset))
    if a.modules:
        import cro_functions
        mrows = [l.rstrip("\n").split("\t") for l in open(a.modules) if l.strip() and not l.startswith("#")]
        for m in sorted(cro_functions.load(a.cro), key=lambda m: m.name):
            rows_ = [r for r in mrows if r[0] == m.name]
            if not rows_:
                continue
            ms = sorted(int(r[1], 16) for r in rows_)
            mt = {int(r[1], 16) for r in rows_ if r[3] == "thumb"}
            msize = {int(r[1], 16): int(r[4]) for r in rows_}
            mf, md, mw, mr, me = translate_program.translate_program(
                m.data, 0, ms, msize, mt, frozenset(m.relocated), m.name, a.jobs)
            mt |= me
            # a branch before the module's file (data read as code, as a string after a function's last branch:
            # DllField's 00031FC0) has no C name: the function making it stays a stub, and so, in turn, does any
            # calling it; one past the file's end becomes a stub like any untranslated target (never reached)
            while True:
                drop = [o for o in mf if any(t < 0 for t in md[o])]
                if not drop:
                    break
                for o in drop:
                    del mf[o]
                    reasons["(module) branches before its file"] = reasons.get("(module) branches before its file", 0) + 1
            mwanted = set(ms) | {t for o, d in md.items() if o in mf for t in d}
            mstubs = sorted(t for t in mwanted if t not in mf)
            for k, v in mr.items():
                reasons[k] = reasons.get(k, 0) + v
            keys_ = sorted(((o | (o in mt)), o) for o in set(mf) | set(mstubs))
            modules.append((m.name, len(m.data), {o: c for o, (c, _) in mf.items()}, mstubs, keys_))
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "functions.h"), "w") as f:
        f.write("/* generated by tools/build_game.py: the game's functions (not to be committed) */\n#include \"Cpu.h\"\n")
        f.write(arm2c.RUNTIME)
        f.writelines("void %s(Cpu* c, uint32_t ret);\n" % arm2c.fname(s) for s in every)
        for name, _, _, _, keys_ in modules:
            f.write("extern uint32_t B_%s;\n" % name)
            f.writelines("void %s(Cpu* c, uint32_t ret);\n" % arm2c.fname(o, name) for _, o in keys_)
    order = sorted(funcs)
    bodies = [funcs[s][0] for s in order] + [c for _, _, mf, _, _ in modules for _, c in sorted(mf.items())]
    parts = max(1, a.jobs, len(bodies) // 1500)  # (files of ~1,500 functions: cc's memory, a phone's too)
    srcs = []
    for k in range(parts):
        path = os.path.join(a.out, "part%d.c" % k)
        with open(path, "w") as f:
            f.write("/* generated by tools/build_game.py (not to be committed) */\n#include \"functions.h\"\n")
            for c in bodies[k::parts]:
                f.write(c + "\n")
        srcs.append(path)
    path = os.path.join(a.out, "table.c")
    with open(path, "w") as f:
        f.write("/* generated by tools/build_game.py (not to be committed) */\n#include \"functions.h\"\n")
        for s in stubs:  # (a stub: the thread stops, its pc where the untranslated function is)
            f.write("void %s(Cpu* c, uint32_t ret) { (void)ret; c->r[15] = 0x%Xu; c->fault = HOS_FAULT_NOT_TRANSLATED; }\n"
                    % (arm2c.fname(s), s))
        keys = sorted(((s | (s in thumb)), s) for s in every)
        f.write("static const uint32_t A[] = {%s};\n" % ", ".join("0x%Xu" % k for k, _ in keys))
        f.write("typedef void (*Fn)(Cpu*, uint32_t);\nstatic const Fn T[] = {%s};\n" % ", ".join(
            arm2c.fname(s) for _, s in keys))
        # the modules: their bases (0 while not loaded), their tables, and the loader's two calls
        f.write("#include <string.h>\n")
        for name, size_, _, mstubs, keys_ in modules:
            f.write("uint32_t B_%s;\n" % name)
            for o in mstubs:
                f.write("void %s(Cpu* c, uint32_t ret) { (void)ret; c->r[15] = B_%s + 0x%Xu; "
                        "c->fault = HOS_FAULT_NOT_TRANSLATED; }\n" % (arm2c.fname(o, name), name, o))
            f.write("static const uint32_t A_%s[] = {%s};\n" % (name, ", ".join("0x%Xu" % k for k, _ in keys_)))
            f.write("static const Fn T_%s[] = {%s};\n" % (name, ", ".join(arm2c.fname(o, name) for _, o in keys_)))
        f.write("static const struct { const char* name; uint32_t* base; uint32_t size; const uint32_t* a; const Fn* t; "
                "int n; } M[] = {%s{0, 0, 0, 0, 0, 0}};\n" % "".join(
                    "{\"%s\", &B_%s, 0x%Xu, A_%s, T_%s, %d}, " % (n, n, sz, n, n, len(k)) for n, sz, _, _, k in modules))
        f.write("static int find(const uint32_t* a, int n, uint32_t t)\n{\n    int lo = 0, hi = n;\n"
                "    while (lo < hi) { int m = (lo + hi) / 2; if (a[m] < t) lo = m + 1; else hi = m; }\n"
                "    return lo < n && a[lo] == t ? lo : -1;\n}\n")
        f.write("int hos_module_loaded(const char* name, uint32_t address)\n{\n"
                "    for (int i = 0; M[i].name; i++) if (!strcmp(M[i].name, name)) { *M[i].base = address; return 1; }\n"
                "    return 0;\n}\n")
        f.write("void hos_module_unloaded(uint32_t address)\n{\n"
                "    for (int i = 0; M[i].name; i++) if (*M[i].base == address) *M[i].base = 0;\n}\n")
        f.write("void call_indirect(Cpu* c, uint32_t t, uint32_t ret)\n{\n    int k = find(A, %d, t);\n"
                "    if (k >= 0) { T[k](c, ret); return; }\n"
                "    for (int i = 0; M[i].name; i++)\n"
                "        if (*M[i].base && t - *M[i].base < M[i].size) {\n"
                "            if ((k = find(M[i].a, M[i].n, t - *M[i].base)) >= 0) { M[i].t[k](c, ret); return; }\n"
                "            break;\n        }\n"
                "    c->r[15] = t; c->fault = HOS_FAULT_NOT_TRANSLATED;\n}\n" % len(keys))
    srcs.append(path)
    trace = ["-DHOS_GPU_TRACE"] if a.gpu_trace else []
    flags = trace + [a.opt, "-w", "-frounding-math", "-ffp-contract=off", "-fno-strict-aliasing", "-I", HOS, "-I", a.out]
    failed, running = [], []
    for src in srcs:  # (at most --jobs at once)
        if len(running) >= a.jobs:
            path_, job = running.pop(0)
            if job.wait():
                failed.append(path_)
        running.append((src, subprocess.Popen([a.cc, "-c"] + flags + ["-o", src + ".o", src])))
    failed += [path_ for path_, job in running if job.wait()]
    lib = os.path.join(a.out, "libgame_code.a")
    report = ["%d functions in the list (%d Thumb); %d translated (%d of them entries), %d stubs"
              % (len(starts), len(thumb - entry_thumb), len(funcs), len(set(funcs) - set(starts)), len(stubs))]
    if modules:
        report.append("%d modules: %d functions translated, %d stubs" % (
            len(modules), sum(len(m[2]) for m in modules), sum(len(m[3]) for m in modules)))
    if failed:
        report.append("DOES NOT COMPILE: %s" % ", ".join(failed))
    else:
        if os.path.exists(lib):
            os.remove(lib)
        subprocess.run(["ar", "rcs", lib] + [s + ".o" for s in srcs], check=True)
        # what the library needs from outside: the symbols its objects use, less those any of them defines
        used, defined = set(), set()
        for line in subprocess.run(["nm", lib], capture_output=True, text=True, check=True).stdout.split("\n"):
            parts_ = line.split()
            if len(parts_) == 2 and parts_[0] == "U":
                used.add(parts_[1])
            elif len(parts_) == 3 and parts_[1] in "TDBRtdbr":
                defined.add(parts_[2])
        undefined = used - defined
        game = sorted(u for u in undefined if u.startswith("f_") or u == "call_indirect")
        report.append("compiled: %s (%d bytes); undefined: the runtime's %s, the C library's %s%s" % (
            lib, os.path.getsize(lib), sorted(undefined & RUNTIME_SYMBOLS),
            sorted(undefined - RUNTIME_SYMBOLS - set(game)), "; MISSING GAME FUNCTIONS: %s" % game if game else ""))
    report.append("stubs, by reason:")
    by = {}
    for s in stubs:
        by.setdefault(why.get(s, "called, not in the list"), []).append(s)
    for w, ss in sorted(by.items(), key=lambda kv: -len(kv[1])):
        report.append("  %5d  %s  (%s)" % (len(ss), w, " ".join("%08X" % x for x in ss[:6])))
    if a.program and not failed:
        root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src")
        runtime = sorted(os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs if f.endswith(".c"))
        game = os.path.join(a.out, "game")
        r = subprocess.run([a.cc, "-std=c11", "-D_GNU_SOURCE", "-O2", "-Wall"] + trace + ["-I", HOS, "-o", game] + runtime +
                           [lib, "-lm", "-lpthread"])
        report.append("program: %s" % (game if r.returncode == 0 else "DOES NOT LINK"))
        failed = failed or [game] if r.returncode else failed
    open(os.path.join(a.out, "build.txt"), "w").write("\n".join(report) + "\n")
    print("\n".join(report))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
