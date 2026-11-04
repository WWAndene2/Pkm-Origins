#!/usr/bin/env python3
"""Step 3b: the translator's first slice, ARM to C, mechanically, for leaf functions (no call, no stack, integer only).
Each instruction becomes C on a register file and the flags; branches inside the function become gotos; literal pool
loads become constants. Anything outside this subset makes the function "unsupported" (returned as None, with the reason),
never translated approximately. Matched against the original by tools/match_leaf.py.

  arm2c.py <code.bin> <address> [--end ADDRESS]      prints the C of one function (or why it is unsupported)

The C uses: typedef struct { uint32_t r[16]; uint32_t n, z, c, v; int fault; uint32_t steps; } Cpu; and the memory helpers ld8/ld16/ld32,
st8/st16/st32 (tools/match_leaf.py defines them: a fault flag instead of undefined behaviour out of the test memory).
"""
import argparse
import struct

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG

REG = {"r0": 0, "r1": 1, "r2": 2, "r3": 3, "r4": 4, "r5": 5, "r6": 6, "r7": 7, "r8": 8, "sb": 9, "r9": 9, "sl": 10,
       "r10": 10, "fp": 11, "r11": 11, "ip": 12, "r12": 12, "sp": 13, "lr": 14, "pc": 15}
COND = {1: "c->z", 2: "!c->z", 3: "c->c", 4: "!c->c", 5: "c->n", 6: "!c->n", 7: "c->v", 8: "!c->v",
        9: "(c->c && !c->z)", 10: "(!c->c || c->z)", 11: "(c->n == c->v)", 12: "(c->n != c->v)",
        13: "(!c->z && c->n == c->v)", 14: "(c->z || c->n != c->v)"}
SHIFT = {1: "asr", 2: "lsl", 3: "lsr", 4: "ror"}


class Unsupported(Exception):
    pass


def operand2(md_insn, op, want_carry):
    """C expression of a flexible second operand, and of its shifter carry (None: the carry flag is unchanged)."""
    if op.type == ARM_OP_IMM:
        v = op.imm & 0xFFFFFFFF
        if want_carry and v > 0xFF:
            raise Unsupported("logical flag-setting with a rotated immediate")
        return "0x%Xu" % v, None
    if op.type != ARM_OP_REG:
        raise Unsupported("operand kind")
    r = "c->r[%d]" % REG[md_insn.reg_name(op.reg)]
    if REG[md_insn.reg_name(op.reg)] == 15:
        raise Unsupported("pc as an operand")
    t, n = op.shift.type, op.shift.value
    if t == 0 or (t == 2 and n == 0):
        return r, None
    if t not in SHIFT or not 0 < n < 32:
        raise Unsupported("shift by register or rrx")
    kind = SHIFT[t]
    expr = {"lsl": "(%s << %d)", "lsr": "(%s >> %d)", "asr": "(uint32_t)((int32_t)%s >> %d)",
            "ror": "((%s >> %d) | (%s << (32 - %d)))"}[kind]
    e = expr % ((r, n, r, n) if kind == "ror" else (r, n))
    carry = "((%s >> %d) & 1)" % (r, 32 - n) if kind == "lsl" else "((%s >> %d) & 1)" % (r, n - 1)
    return e, carry


def translate(code, base, start, end, name):
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    word = lambda a: struct.unpack_from("<I", code, a - base)[0]
    insns = []
    for a in range(start, end, 4):
        got = list(md.disasm(code[a - base:a - base + 4], a))
        if not got:
            break  # data after the function's last instruction
        insns.append(got[0])
    body, targets, returned = [], set(), False
    for i in insns:
        m = i.mnemonic
        base_m = m[:-2] if i.cc not in (0, 15) else m  # the condition suffix (bxne, addseq)
        base_m = base_m[:-1] if base_m.endswith("s") and i.update_flags else base_m
        ops = i.operands
        rd = lambda k=0: REG[i.reg_name(ops[k].reg)]
        line = []
        if base_m == "bx" and i.reg_name(ops[0].reg) == "lr":
            line.append("return;")
            returned = i.cc in (0, 15)
        elif base_m == "b":
            t = ops[0].imm
            if not start <= t < start + 4 * len(insns):
                raise Unsupported("branch out of the function (tail call)")
            targets.add(t)
            line.append("goto L%X;" % t)
        elif base_m in ("lsl", "lsr", "asr", "ror") and len(ops) == 3 and ops[2].type == ARM_OP_IMM:
            n = ops[2].imm
            if rd() == 15 or not 0 < n < 32:
                raise Unsupported("shift form")
            r = "c->r[%d]" % REG[i.reg_name(ops[1].reg)]
            e = {"lsl": "(%s << %d)" % (r, n), "lsr": "(%s >> %d)" % (r, n), "asr": "(uint32_t)((int32_t)%s >> %d)" % (r, n),
                 "ror": "((%s >> %d) | (%s << (32 - %d)))" % (r, n, r, n)}[base_m]
            carry = "((%s >> %d) & 1)" % (r, 32 - n) if base_m == "lsl" else "((%s >> %d) & 1)" % (r, n - 1)
            line.append("{ uint32_t k = %s, v = %s; c->r[%d] = v;" % (carry, e, rd()))
            if i.update_flags:
                line.append(" c->n = v >> 31; c->z = v == 0; c->c = k;")
            line.append(" (void)k; }")
        elif base_m in ("mov", "mvn"):
            if rd() == 15:
                raise Unsupported("write to pc")
            e, carry = operand2(i, ops[1], i.update_flags)
            v = e if base_m == "mov" else "~(%s)" % e
            line.append("{ uint32_t v = %s; c->r[%d] = v;" % (v, rd()))
            if i.update_flags:
                line.append(" c->n = v >> 31; c->z = v == 0;" + (" c->c = %s;" % carry if carry else ""))
            line.append(" }")
        elif base_m in ("add", "sub", "rsb", "cmp", "cmn", "and", "orr", "eor", "bic", "tst", "teq"):
            compare = base_m in ("cmp", "cmn", "tst", "teq")
            logical = base_m in ("and", "orr", "eor", "bic", "tst", "teq")
            dst = None if compare else rd()
            if dst == 15:
                raise Unsupported("write to pc")
            a_op, b_op = (ops[0], ops[1]) if compare else (ops[1], ops[2] if len(ops) > 2 else None)
            if b_op is None:
                raise Unsupported("two-operand form")
            if i.reg_name(a_op.reg) == "pc":
                raise Unsupported("pc-relative address")
            ra = "c->r[%d]" % REG[i.reg_name(a_op.reg)]
            flags = compare or i.update_flags
            e, carry = operand2(i, b_op, flags and logical)
            x, y = (e, ra) if base_m == "rsb" else (ra, e)
            line.append("{ uint32_t x = %s, y = %s, v;" % (x, y))
            if base_m in ("add", "cmn"):
                line.append(" v = x + y;")
                if flags:
                    line.append(" c->c = v < x; c->v = ((~(x ^ y) & (x ^ v)) >> 31);")
            elif base_m in ("sub", "rsb", "cmp"):
                line.append(" v = x - y;")
                if flags:
                    line.append(" c->c = x >= y; c->v = (((x ^ y) & (x ^ v)) >> 31);")
            else:
                op = {"and": "x & y", "tst": "x & y", "orr": "x | y", "eor": "x ^ y", "teq": "x ^ y", "bic": "x & ~y"}[base_m]
                line.append(" v = %s;" % op)
                if flags and carry:
                    line.append(" c->c = %s;" % carry)
            if flags:
                line.append(" c->n = v >> 31; c->z = v == 0;")
            if not compare:
                line.append(" c->r[%d] = v;" % dst)
            line.append(" }")
        elif base_m == "mul" and not i.update_flags:
            line.append("c->r[%d] = c->r[%d] * c->r[%d];" % (rd(), REG[i.reg_name(ops[1].reg)], REG[i.reg_name(ops[2].reg)]))
        elif base_m in ("ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "str", "strb", "strh"):
            mem = ops[1]
            if mem.type != ARM_OP_MEM or i.writeback or len(ops) > 2:
                raise Unsupported("addressing with writeback")
            if rd() == 15:
                raise Unsupported("load to pc")
            basereg = i.reg_name(mem.mem.base)
            if basereg == "pc":
                if base_m != "ldr":
                    raise Unsupported("pc-relative non-word access")
                line.append("c->r[%d] = 0x%Xu;" % (rd(), word(i.address + 8 + mem.mem.disp)))
            else:
                addr = "c->r[%d]" % REG[basereg]
                if mem.mem.index:
                    if mem.shift.type not in (0,) and not (mem.shift.type == 2):
                        raise Unsupported("shifted index")
                    idx = "c->r[%d]" % REG[i.reg_name(mem.mem.index)]
                    if mem.shift.type == 2:
                        idx = "(%s << %d)" % (idx, mem.shift.value)
                    addr += (" - " if mem.subtracted else " + ") + idx
                elif mem.mem.disp:
                    addr += " + 0x%Xu" % (mem.mem.disp & 0xFFFFFFFF) if mem.mem.disp > 0 else " - 0x%Xu" % -mem.mem.disp
                load = {"ldr": "ld32(c, %s)", "ldrb": "ld8(c, %s)", "ldrh": "ld16(c, %s)",
                        "ldrsb": "(uint32_t)(int8_t)ld8(c, %s)", "ldrsh": "(uint32_t)(int16_t)ld16(c, %s)"}
                if base_m in load:
                    line.append("c->r[%d] = %s;" % (rd(), load[base_m] % addr))
                else:
                    line.append("st%s(c, %s, c->r[%d]);" % ({"str": "32", "strb": "8", "strh": "16"}[base_m], addr, rd()))
            line.append(" if (c->fault) return;")
        else:
            raise Unsupported("instruction %s" % m)
        stmt = "".join(line)
        if i.cc not in (0, 15):
            stmt = "if (%s) { %s }" % (COND[i.cc], stmt)
        body.append((i.address, "    /* %08X %s %s */ %s" % (i.address, m, i.op_str, stmt)))
        if returned and max(targets, default=0) <= i.address:
            break
        returned = False
    if not returned:
        raise Unsupported("no unconditional return found")
    out = ["void %s(Cpu* c)\n{" % name]
    for a, s in body:
        if a in targets:
            out.append("L%X: if (++c->steps > 100000) { c->fault = 2; return; }" % a)
        out.append(s)
    out.append("}\n")
    return "\n".join(out), 4 * len(body)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("code")
    p.add_argument("address", type=lambda s: int(s, 0))
    p.add_argument("--end", type=lambda s: int(s, 0))
    p.add_argument("--base", type=lambda s: int(s, 0), default=0x100000)
    a = p.parse_args()
    code = open(a.code, "rb").read()
    try:
        print(translate(code, a.base, a.address, a.end or a.address + 0x400, "f_%08X" % a.address)[0])
    except Unsupported as e:
        print("unsupported:", e)


if __name__ == "__main__":
    main()
