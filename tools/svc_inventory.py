#!/usr/bin/env python3
"""What the game asks of the 3DS system (docs/RECOMPILATION_PLAN.md §10, step 5: the input of `src/hos/`): every svc
of the program and its modules, by call number and the functions making it, and the IPC requests the game sends with
svc 0x32 (SendSyncRequest), by command header.

  svc_inventory.py <code.bin> <functions.tsv> <cro dir> <cro functions.tsv> <azahar dir>

Names are Azahar's (read from its source, never written by hand): the system calls from
src/core/hle/kernel/svc.cpp's table (with whether Azahar implements each), the IPC commands from the FunctionInfo
tables of src/core/hle/service/ (the service each table belongs to: the ServiceFramework name before it).
An IPC request is found where the function writes a constant to its command buffer: the thread-local storage pointer
(mrc p15, 0, rX, c13, c0, 3) + 0x80, the header being the buffer's first word (command id << 16, normal parameter
words << 6, translate words). Which service a request goes to is the handle's, known only when run: a header is listed
with every service whose table has its command id, and the requests whose header is not a constant the scan follows
are counted apart (no guess). Prints a report; writes nothing.
"""
import argparse
import collections
import os
import re
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cro_functions  # noqa: E402
import function_list  # noqa: E402
from arm2c import REG  # noqa: E402


def azahar_svcs(azahar):
    """{number: (name, implemented)} from svc.cpp's table"""
    out = {}
    text = open(os.path.join(azahar, "src/core/hle/kernel/svc.cpp")).read()
    for m in re.finditer(r'\{0x([0-9A-Fa-f]{2}), (nullptr|&[^,]+(?:,\s*0x[0-9A-Fa-f]+>)?), "(\w+)"', text):
        out[int(m.group(1), 16)] = (m.group(3), m.group(2) != "nullptr")
    return out


def azahar_commands(azahar):
    """{command id: [(service, name, implemented)]} from the services' FunctionInfo tables"""
    out = collections.defaultdict(list)
    root = os.path.join(azahar, "src/core/hle/service")
    for d, _, files in os.walk(root):
        for f in files:
            if not f.endswith(".cpp"):
                continue
            text = open(os.path.join(d, f), errors="replace").read()
            names = [(m.start(), m.group(1)) for m in re.finditer(r'ServiceFramework\("([^"]+)"', text)]
            for t in re.finditer(r"FunctionInfo functions\[\] = \{(.*?)\n\s*\};", text, re.S):
                before = [n for at, n in names if at < t.start()]
                service = before[-1] if before else "?(%s)" % f
                for m in re.finditer(r'\{0x([0-9A-Fa-f]+), (nullptr|[^,]+), "(\w+)"\}', t.group(1)):
                    out[int(m.group(1), 16)].append((service, m.group(3), m.group(2) != "nullptr"))
    return out


def data_words(data, base, start, end, thumb, word):
    """the places of the function its pc-relative loads read (literal pools), from its code only, as tools/arm2c.py
    sets them aside: ARM words (repeated until it settles: a pool word can read as a load), Thumb halfwords"""
    if thumb:
        out = set()
        for a in range(start, end - 1, 2):
            h = struct.unpack_from("<H", data, a - base)[0]
            if h >> 11 == 0x09:
                t = ((a + 4) & ~3) + (h & 0xFF) * 4
                out.update((t, t + 2))
        return out
    out = set()
    for _ in range(16):
        new = set()
        for a in range(start, end - 3, 4):
            if a not in out:
                new.update(function_list.pc_relative_words(word(a), a))
        if new == out:
            break
        out = new
    return out


def scan(md, data, base, start, size, thumb, word):
    """(svc numbers, IPC headers, requests with no constant header) of one function"""
    svcs, headers, unknown, pending = [], [], 0, False  # pending: a header written since the last svc
    const, tls, buf = {}, set(), set()  # register -> constant; registers holding the TLS pointer / the command buffer
    a, end = start, start + size
    pool = data_words(data, base, start, end, thumb, word)
    targets = set()
    while a < end:
        if a in pool:  # a literal pool word: data, not decoded (a pool word can read as an svc)
            a += 2 if thumb else 4
            continue
        got = list(md.disasm(data[a - base:a - base + 4], a))
        if not got:
            a += 2 if thumb else 4
            continue
        i = got[0]
        a += i.size
        m, ops = i.mnemonic, i.operands
        regs = [REG.get(i.reg_name(o.reg)) for o in ops if o.type == ARM_OP_REG]
        # control flow: only code a branch reaches is read (strings and tables sit between a function's pieces)
        if m[0] == "b" and not m.startswith(("bl", "bic")) and ops and ops[0].type == ARM_OP_IMM:
            if start <= ops[0].imm < end:
                targets.add(ops[0].imm)
        final = i.cc in (0, 15) and (m == "b" or m.startswith("bx") or (m.startswith("pop") or m.startswith("ldm"))
                                    and "pc" in i.op_str or m == "ldr" and i.op_str.startswith("pc"))
        if final:
            later = [t for t in targets if t >= a]
            if not later:
                break
            a = min(later)
        if m.startswith("svc"):
            n = ops[0].imm & 0xFFFFFF
            svcs.append(n)
            if n == 0x32 and not pending:
                unknown += 1
            pending = False
            const, tls, buf = {}, set(), set()  # (the call returns its own values)
            continue
        if m.startswith("mrc") and "c13, c0, #3" in i.op_str:
            r = REG.get(i.op_str.split(",")[2].strip())
            tls.add(r)
            const.pop(r, None)
            continue
        dst = regs[0] if regs else None
        if m in ("ldr",) and len(ops) == 2 and ops[1].type == ARM_OP_MEM and i.reg_name(ops[1].mem.base) == "pc":
            t = ((i.address + 4) & ~3 if thumb else i.address + 8) + ops[1].mem.disp
            if base <= t < base + len(data) - 3:
                const[dst] = word(t)
                tls.discard(dst)
                buf.discard(dst)
            continue
        if m in ("mov", "movs", "movw") and len(ops) == 2 and ops[1].type == ARM_OP_IMM:
            const[dst] = ops[1].imm & 0xFFFFFFFF
            tls.discard(dst)
            buf.discard(dst)
            continue
        if m in ("add", "adds") and len(ops) == 3 and ops[2].type == ARM_OP_IMM and regs[1] in tls and ops[2].imm == 0x80:
            buf.add(dst)
            const.pop(dst, None)
            continue
        if m == "str" and len(ops) == 2 and ops[1].type == ARM_OP_MEM:
            b, d = REG.get(i.reg_name(ops[1].mem.base)), ops[1].mem.disp
            if (b in tls and d == 0x80 or b in buf and d == 0) and dst in const:
                headers.append((const[dst], i.address))
                pending = True
            continue
        if m.startswith(("str", "cmp", "tst", "cmn", "teq", "b", "push", "vst")):
            continue
        for r in regs[:1]:  # any other instruction writing its first register: what the scan knew of it is gone
            const.pop(r, None)
            tls.discard(r)
            buf.discard(r)
    return svcs, headers, unknown


def main():
    p = argparse.ArgumentParser()
    for name in ("code", "functions", "cro_dir", "cro_functions", "azahar"):
        p.add_argument(name)
    a = p.parse_args()
    svc_names, commands = azahar_svcs(a.azahar), azahar_commands(a.azahar)
    md = {False: Cs(CS_ARCH_ARM, CS_MODE_ARM), True: Cs(CS_ARCH_ARM, CS_MODE_THUMB)}
    for d in md.values():
        d.detail = True
    places = []  # (data, base, place name, start, size, thumb)
    code = open(a.code, "rb").read()
    for line in open(a.functions):
        if not line.startswith("#"):
            f = line.split()
            places.append((code, 0x100000, "", int(f[0], 16), int(f[4]), f[3] == "thumb"))
    mods = {m.name: m.data for m in cro_functions.load(a.cro_dir)}
    for line in open(a.cro_functions):
        if not line.startswith("#"):
            f = line.split()
            places.append((mods[f[0]], 0, f[0] + "+", int(f[1], 16), int(f[4]), f[3] == "thumb"))
    by_svc, by_header, unknown = collections.defaultdict(set), collections.defaultdict(set), 0
    for data, base, where, start, size, thumb in places:
        word = lambda x, d=data, b=base: struct.unpack_from("<I", d, x - b)[0]
        svcs, headers, u = scan(md[thumb], data, base, start, size, thumb, word)
        name = "%s%08X" % (where, start) if not where else "%s%06X" % (where, start)
        for n in svcs:
            by_svc[n].add(name)
        for h, _ in headers:
            by_header[h].add(name)
        unknown += u
    print("system calls (svc), by number: functions making it, Azahar's name (* not implemented in Azahar)")
    for n in sorted(by_svc):
        nm, impl = svc_names.get(n, ("?", False))
        print("  0x%02X  %4d  %s%s  (e.g. %s)" % (n, len(by_svc[n]), nm, "" if impl else " *", min(by_svc[n])))
    print("IPC requests (svc 0x32), by header: functions sending it, command id, normal / translate words, services "
          "whose table has that id (* not implemented in Azahar)")
    for h in sorted(by_header, key=lambda h: (h >> 16, h)):
        cmd = h >> 16
        c = commands.get(cmd, [])
        cands = ", ".join("%s %s%s" % (s, nm, "" if impl else " *") for s, nm, impl in c) if len(c) <= 3 else \
            "%d services have this id: which one is the handle's (a run tells)" % len(c)
        print("  %08X  %3d  0x%04X  %d / %d  %s" % (h, len(by_header[h]), cmd, (h >> 6) & 0x3F, h & 0x3F,
                                                  cands or "(no service table has it)"))
    print("%d system call numbers, %d IPC headers; %d svc 0x32 whose header is not a constant the scan follows"
          % (len(by_svc), len(by_header), unknown))


if __name__ == "__main__":
    main()
