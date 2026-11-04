#!/usr/bin/env python3
"""Which registers each system call takes and returns (for src/hos/Kernel.c), computed from Azahar's own declarations
the way its svc_wrapper.h does, never written by hand:

  svc_abi.py <azahar dir> [svc number...]

Azahar's rule (src/core/hle/kernel/svc_wrapper.h, SVCWrapper::GetSVCABI): the inputs are laid out as the ARM calling
convention lays out the call's parameters (one word each, a 64-bit one in an even pair, r0-r3 then what would go on
the stack), outputs (pointer parameters) removed, and whatever would be on the stack moved into the registers left
free from r4 on; the outputs are r0 (the Result, when the call returns one) then each output word in order.
"""
import argparse
import os
import re

WORDS = {"u8": 1, "s8": 1, "u16": 1, "s16": 1, "u32": 1, "s32": 1, "Handle": 1, "VAddr": 1, "PAddr": 1, "bool": 1,
         "u64": 2, "s64": 2, "MemoryInfo": 4, "PageInfo": 1}


def signatures(azahar):
    text = open(os.path.join(azahar, "src/core/hle/kernel/svc.cpp")).read()
    out = {}
    for m in re.finditer(r"^(Result|void|s64|u64|u32) SVC::(\w+)\(([^)]*)\)", text, re.M):
        params = []
        for p in [x.strip() for x in m.group(3).replace("\n", " ").split(",") if x.strip()]:
            t = p.rsplit(" ", 1)[0].replace("const ", "").strip()
            params.append((t.rstrip("*").strip(), t.endswith("*"), p.rsplit(" ", 1)[1].lstrip("*")))
        out[m.group(2)] = (m.group(1), params)
    table = {}
    for m in re.finditer(r'\{0x([0-9A-Fa-f]{2}), &SVC::(?:Wrap<&SVC::)?(\w+)', text):
        table[int(m.group(1), 16)] = m.group(2)
    return out, table


def abi(ret, params):
    """(inputs: [(register, name, word)], outputs: [(register, name, word)])"""
    regs, stack, pos = [], [], 0
    for t, is_out, name in params:
        n = WORDS[t]
        if t in ("u64", "s64"):
            pos += pos % 2
        for w in range(n):
            (regs if pos < 4 else stack).append((pos, t, is_out, name, w))
            pos += 1
    ins, outs = [], []
    o = 0
    if ret != "void":
        for w in range(WORDS.get(ret, 1) if ret != "Result" else 1):
            outs.append((o, "result" if ret == "Result" else "return", w))
            o += 1
    for t, is_out, name in params:
        if is_out:
            for w in range(WORDS[t]):
                outs.append((o, name, w))
                o += 1
    stack_in = [s for s in stack if not s[2]]
    for k in range(8):
        slot = next((r for r in regs if r[0] == k), None)
        if slot is not None and not slot[2]:
            ins.append((k, slot[3], slot[4]))
        elif stack_in:
            s = stack_in.pop(0)
            ins.append((k, s[3], s[4]))
    return ins, outs


def main():
    p = argparse.ArgumentParser()
    p.add_argument("azahar")
    p.add_argument("numbers", nargs="*", type=lambda v: int(v, 16))
    a = p.parse_args()
    sigs, table = signatures(a.azahar)
    for n in sorted(a.numbers or table):
        name = table.get(n)
        if name not in sigs:
            print("0x%02X %s: no declaration found" % (n, name))
            continue
        ins, outs = abi(*sigs[name])
        word = lambda nm, w, t: nm if t == 1 else "%s.%s" % (nm, "lo" if w == 0 else "hi" if t == 2 else w)
        sizes = {nm: WORDS.get(t, 1) for t, _, nm in sigs[name][1]}
        sizes["result"] = sizes["return"] = 1
        print("0x%02X %-30s in: %s   out: %s" % (
            n, name, ", ".join("r%d=%s" % (r, word(nm, w, sizes[nm])) for r, nm, w in ins) or "-",
            ", ".join("r%d=%s" % (r, word(nm, w, sizes[nm])) for r, nm, w in outs) or "-"))


if __name__ == "__main__":
    main()
