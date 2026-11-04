#!/usr/bin/env python3
"""The translator, ARM to C, mechanically (steps 3 and 5 of docs/RECOMPILATION_PLAN.md §10): integer instructions, the
stack (push / pop, ldm / stm, writeback addressing) and direct calls and tail calls to other translated functions.
Each instruction becomes C on a register file and the flags; branches inside the function become gotos; literal pool
loads become constants. Anything outside this subset makes the function "unsupported" (returned as None, with the reason),
never translated approximately. Matched against the original by tools/match_functions.py.

  arm2c.py <code.bin> <address> [--end ADDRESS]      prints the C of one function (or why it is unsupported)

The C uses: typedef struct { uint32_t r[16]; uint32_t n, z, c, v; int fault; uint32_t steps; uint32_t s[32], fpscr; } Cpu; and the memory helpers ld8/ld16/ld32,
st8/st16/st32 (tools/match_functions.py defines them: a fault flag instead of undefined behaviour out of the test memory).
"""
import argparse
import os
import re
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import function_list  # noqa: E402

REG = {"r0": 0, "r1": 1, "r2": 2, "r3": 3, "r4": 4, "r5": 5, "r6": 6, "r7": 7, "r8": 8, "sb": 9, "r9": 9, "sl": 10,
       "r10": 10, "fp": 11, "r11": 11, "ip": 12, "r12": 12, "sp": 13, "lr": 14, "pc": 15}
COND = {1: "c->z", 2: "!c->z", 3: "c->c", 4: "!c->c", 5: "c->n", 6: "!c->n", 7: "c->v", 8: "!c->v",
        9: "(c->c && !c->z)", 10: "(!c->c || c->z)", 11: "(c->n == c->v)", 12: "(c->n != c->v)",
        13: "(!c->z && c->n == c->v)", 14: "(c->z || c->n != c->v)"}
SHIFT = {1: "asr", 2: "lsl", 3: "lsr", 4: "ror"}


class Unsupported(Exception):
    pass


# C the translated functions use besides the memory helpers: shifts by a register (ARM's rules for amounts 0 and 32 and
# more), and call_indirect(c, target, ret), the lookup of a translated function by address (defined with the table of
# functions: a target with no translation sets c->fault = 4)
RUNTIME = r"""
static inline uint32_t shreg(Cpu* c, int kind, uint32_t x, uint32_t s, uint32_t* k)
{
    uint32_t n = s & 0xFF;
    *k = c->c;
    if (n == 0) return x;
    switch (kind) {
    case 0: if (n < 32) { *k = (x >> (32 - n)) & 1; return x << n; } *k = n == 32 ? x & 1 : 0; return 0;
    case 1: if (n < 32) { *k = (x >> (n - 1)) & 1; return x >> n; } *k = n == 32 ? x >> 31 : 0; return 0;
    case 2: if (n < 32) { *k = (x >> (n - 1)) & 1; return (uint32_t)((int32_t)x >> n); } *k = x >> 31;
            return (uint32_t)((int32_t)x >> 31);
    default: n &= 31; if (n == 0) { *k = x >> 31; return x; } *k = (x >> (n - 1)) & 1; return (x >> n) | (x << (32 - n));
    }
}
void call_indirect(Cpu* c, uint32_t t, uint32_t ret);
/* VFP, as the game runs it: FPSCR 0x03C00010 (Azahar's kernel/thread.cpp, as the 3DS kernel sets each thread): default
   NaN, flush to zero, round toward zero. The registers are bits (s[32]; d[k] is s[2k] low, s[2k+1] high); arithmetic
   runs on the host's float / double with its rounding set toward zero (match_functions.py: fesetround, compiled with
   -frounding-math -ffp-contract=off, so nothing is folded or fused), and these helpers add ARM's other two rules: a
   denormal input or result is a zero of its sign (under round toward zero a result tiny before rounding stays tiny
   after, so flushing the host's result is ARM's flush), and any NaN result is the default NaN. */
static inline float F32(uint32_t x) { float f; memcpy(&f, &x, 4); return f; }
static inline double F64(uint64_t x) { double f; memcpy(&f, &x, 8); return f; }
static inline uint32_t fz32(uint32_t x) { return (x & 0x7F800000u) == 0 ? x & 0x80000000u : x; }
static inline uint64_t fz64(uint64_t x) { return (x & 0x7FF0000000000000ull) == 0 ? x & 0x8000000000000000ull : x; }
static inline float in32(uint32_t x) { return F32(fz32(x)); }
static inline double in64(uint64_t x) { return F64(fz64(x)); }
static inline uint32_t out32(float f) { uint32_t x; if (f != f) return 0x7FC00000u; memcpy(&x, &f, 4); return fz32(x); }
static inline uint64_t out64(double f) { uint64_t x; if (f != f) return 0x7FF8000000000000ull; memcpy(&x, &f, 8); return fz64(x); }
static inline uint64_t rd64(Cpu* c, int k) { return (uint64_t)c->s[2 * k + 1] << 32 | c->s[2 * k]; }
static inline void wr64(Cpu* c, int k, uint64_t v) { c->s[2 * k] = (uint32_t)v; c->s[2 * k + 1] = (uint32_t)(v >> 32); }
/* vcmp: FPSCR's N Z C V (equal 0110, less 1000, greater 0010, unordered 0011) */
static inline void fcmp(Cpu* c, double x, double y)
{
    uint32_t f = x != x || y != y ? 0x3u : x == y ? 0x6u : x < y ? 0x8u : 0x2u;
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | f << 28;
}
/* vcvt to an integer: toward zero, saturating, NaN to 0 */
static inline uint32_t f2s(double x) { if (x != x) return 0; if (x >= 2147483647.0) return 0x7FFFFFFFu;
    if (x <= -2147483648.0) return 0x80000000u; return (uint32_t)(int32_t)x; }
static inline uint32_t f2u(double x) { if (x != x || x <= 0) return 0; if (x >= 4294967295.0) return 0xFFFFFFFFu;
    return (uint32_t)x; }
"""
SHIFT_REG = {7: 0, 8: 1, 6: 2, 9: 3}  # capstone's lsl / lsr / asr / ror by register -> shreg's kind


def operand2(md_insn, op, want_carry):
    """C expression of a flexible second operand, and of its shifter carry (None: the carry flag is unchanged)."""
    if op.type == ARM_OP_IMM:
        v = op.imm & 0xFFFFFFFF
        if want_carry and v > 0xFF:  # a rotated immediate: the carry out is its bit 31 (ARM ARM, A5.1.3)
            return "0x%Xu" % v, "%du" % (v >> 31)
        return "0x%Xu" % v, None
    if op.type != ARM_OP_REG:
        raise Unsupported("operand kind")
    r = "c->r[%d]" % REG[md_insn.reg_name(op.reg)]
    if REG[md_insn.reg_name(op.reg)] == 15:
        raise Unsupported("pc as an operand")
    t, n = op.shift.type, op.shift.value
    if t == 0 or (t == 2 and n == 0):
        return r, None
    if t in SHIFT_REG:  # by a register: its low byte, with ARM's rules (the caller declares sk, the carry out)
        s = REG[md_insn.reg_name(n)]
        if s == 15:
            raise Unsupported("pc as an operand")
        return "shreg(c, %d, %s, c->r[%d], &sk)" % (SHIFT_REG[t], r, s), "sk"
    if t == 5:  # rrx: the carry in at the top, bit 0 out
        return "((c->c << 31) | (%s >> 1))" % r, "(%s & 1)" % r
    if t not in SHIFT or not 0 < n < 32:
        raise Unsupported("shift form")
    kind = SHIFT[t]
    expr = {"lsl": "(%s << %d)", "lsr": "(%s >> %d)", "asr": "(uint32_t)((int32_t)%s >> %d)",
            "ror": "((%s >> %d) | (%s << (32 - %d)))"}[kind]
    e = expr % ((r, n, r, n) if kind == "ror" else (r, n))
    carry = "((%s >> %d) & 1)" % (r, 32 - n) if kind == "lsl" else "((%s >> %d) & 1)" % (r, n - 1)
    return e, carry


MULTI = {"push": ("db", True, False), "pop": ("ia", True, True),
         "stm": ("ia", None, False), "stmib": ("ib", None, False), "stmda": ("da", None, False), "stmdb": ("db", None, False),
         "ldm": ("ia", None, True), "ldmib": ("ib", None, True), "ldmda": ("da", None, True), "ldmdb": ("db", None, True)}


def static_relocations(cro_dir):
    """the words of the .code the loader rewrites when it links the modules: static.crs's external relocations (the
    imports of the program from the modules; Omega Ruby: 367 in the text, measured 4 November)"""
    import cro_functions
    static = cro_functions.Module(os.path.join(cro_dir, "static.crs"))
    out = set()
    for k in range(static.h["ExternalRelocationNum"]):
        tag = struct.unpack_from("<I", static.data, static.h["ExternalRelocationTableOffset"] + 12 * k)[0]
        t = static.tag(tag)
        if t is not None:
            out.add(t)
    return frozenset(out)


def vfp(i, word, relocated):
    """the C of one VFP instruction (its condition is added by the caller), or Unsupported"""
    parts = i.mnemonic.split(".")
    op = parts[0][:-2] if i.cc not in (0, 15) else parts[0]
    types = parts[1:]
    ops = i.operands
    names = [i.reg_name(o.reg) if o.type == ARM_OP_REG else None for o in ops]

    def sreg(k):
        n = names[k]
        if n is None or not re.match(r"s\d+$", n):
            raise Unsupported("instruction %s" % i.mnemonic)
        return int(n[1:])

    def dreg(k):
        n = names[k]
        if n is None or not re.match(r"d\d+$", n) or int(n[1:]) > 15:
            raise Unsupported("instruction %s" % i.mnemonic)
        return int(n[1:])

    def core(k):
        n = names[k]
        if n not in REG or REG[n] in (13, 15):
            raise Unsupported("instruction %s" % i.mnemonic)
        return REG[n]

    if op in ("vldr", "vstr"):
        mem = ops[1]
        if mem.type != ARM_OP_MEM or mem.mem.index:
            raise Unsupported("instruction %s" % i.mnemonic)
        double = names[0].startswith("d")
        k = dreg(0) if double else sreg(0)
        basereg = i.reg_name(mem.mem.base)
        d = mem.mem.disp if not mem.subtracted else -mem.mem.disp
        if basereg == "pc":
            if op == "vstr":
                raise Unsupported("instruction %s" % i.mnemonic)
            t = i.address + 8 + d
            words = []
            for x in ((t, t + 4) if double else (t,)):
                words.append("ld32(c, 0x%Xu)" % x if x in relocated else "0x%Xu" % word(x))
            if double:
                return "c->s[%d] = %s; c->s[%d] = %s; if (c->fault) return;" % (2 * k, words[0], 2 * k + 1, words[1])
            return "c->s[%d] = %s; if (c->fault) return;" % (k, words[0])
        a = "c->r[%d]%s" % (REG[basereg], (" + 0x%Xu" % d) if d > 0 else (" - 0x%Xu" % -d) if d < 0 else "")
        if op == "vldr":
            if double:
                return ("{ uint32_t a = %s; uint32_t v0 = ld32(c, a), v1 = ld32(c, a + 4u); if (c->fault) return;"
                        " c->s[%d] = v0; c->s[%d] = v1; }" % (a, 2 * k, 2 * k + 1))
            return "{ uint32_t v = ld32(c, %s); if (c->fault) return; c->s[%d] = v; }" % (a, k)
        if double:
            return ("{ uint32_t a = %s; st32(c, a, c->s[%d]); st32(c, a + 4u, c->s[%d]); if (c->fault) return; }"
                    % (a, 2 * k, 2 * k + 1))
        return "st32(c, %s, c->s[%d]); if (c->fault) return;" % (a, k)
    if op in ("vpush", "vpop", "vldmia", "vstmia", "vldmdb", "vstmdb"):
        if op in ("vpush", "vpop"):
            b, regs, wb = 13, names, True
            mode, load = ("db", False) if op == "vpush" else ("ia", True)
        else:
            b, regs, wb = REG[names[0]], names[1:], i.writeback
            mode, load = op[4:], op.startswith("vld")
        if b == 15 or mode == "db" and not wb:
            raise Unsupported("instruction %s" % i.mnemonic)
        words = []  # the s registers, in memory order
        for n in regs:
            if n is None or not re.match(r"[sd]\d+$", n) or n[0] == "d" and int(n[1:]) > 15:
                raise Unsupported("instruction %s" % i.mnemonic)
            words += [int(n[1:])] if n[0] == "s" else [2 * int(n[1:]), 2 * int(n[1:]) + 1]
        size = 4 * len(words)
        out = ["{ uint32_t a = c->r[%d]%s;" % (b, " - %du" % size if mode == "db" else "")]
        for k, r in enumerate(words):
            out.append(" uint32_t v%d = ld32(c, a + %du);" % (k, 4 * k) if load else " st32(c, a + %du, c->s[%d]);" % (4 * k, r))
        out.append(" if (c->fault) return;")
        if wb:
            out.append(" c->r[%d] %s= %du;" % (b, "+" if mode == "ia" else "-", size))
        if load:
            out += [" c->s[%d] = v%d;" % (r, k) for k, r in enumerate(words)]
        return "".join(out) + " }"
    if op == "vmov":
        if len(ops) == 2 and names[0] and names[1]:
            a, b = names
            if a[0] == "s" and b[0] == "s":
                return "c->s[%d] = c->s[%d];" % (sreg(0), sreg(1))
            if a[0] == "d" and b[0] == "d":
                return "wr64(c, %d, rd64(c, %d));" % (dreg(0), dreg(1))
            if a[0] == "s":
                return "c->s[%d] = c->r[%d];" % (sreg(0), core(1))
            if b[0] == "s":
                return "c->r[%d] = c->s[%d];" % (core(0), sreg(1))
        if len(ops) == 3 and names[2] and names[2][0] == "d":  # vmov rt, rt2, dm
            return "{ uint32_t lo = c->s[%d], hi = c->s[%d]; c->r[%d] = lo; c->r[%d] = hi; }" % (
                2 * dreg(2), 2 * dreg(2) + 1, core(0), core(1))
        if len(ops) == 3 and names[0] and names[0][0] == "d":  # vmov dm, rt, rt2
            return "c->s[%d] = c->r[%d]; c->s[%d] = c->r[%d];" % (2 * dreg(0), core(1), 2 * dreg(0) + 1, core(2))
        raise Unsupported("instruction vmov form")
    if op == "vmrs":
        if names[0] == "apsr_nzcv":
            return "c->n = c->fpscr >> 31; c->z = (c->fpscr >> 30) & 1; c->c = (c->fpscr >> 29) & 1; c->v = (c->fpscr >> 28) & 1;"
        raise Unsupported("instruction vmrs")  # the whole FPSCR holds sticky exception bits the C does not keep
    if not types or types[-1] not in ("f32", "f64") and op != "vcvt":
        raise Unsupported("instruction %s" % i.mnemonic)
    if op == "vcvt":
        to, frm = types
        dst = dreg(0) if to == "f64" else sreg(0)
        src = dreg(1) if frm == "f64" else sreg(1)
        x = "in64(rd64(c, %d))" % src if frm == "f64" else "in32(c->s[%d])" % src if frm == "f32" else \
            "(int32_t)c->s[%d]" % src if frm == "s32" else "c->s[%d]" % src if frm == "u32" else None
        if x is None:
            raise Unsupported("instruction %s" % i.mnemonic)
        if to == "f64":
            return "wr64(c, %d, out64((double)%s));" % (dst, x)
        if to == "f32":
            return "c->s[%d] = out32((float)%s);" % (dst, x)
        if to in ("s32", "u32") and frm in ("f32", "f64"):
            return "c->s[%d] = f2%s((double)%s);" % (dst, to[0], x)
        raise Unsupported("instruction %s" % i.mnemonic)
    if len(types) != 1:
        raise Unsupported("instruction %s" % i.mnemonic)
    w = 64 if types[0] == "f64" else 32
    reg = dreg if w == 64 else sreg
    get = (lambda k: "in64(rd64(c, %d))" % reg(k)) if w == 64 else (lambda k: "in32(c->s[%d])" % reg(k))
    raw = (lambda k: "rd64(c, %d)" % reg(k)) if w == 64 else (lambda k: "c->s[%d]" % reg(k))
    put = (lambda v: "wr64(c, %d, %s);" % (reg(0), v)) if w == 64 else (lambda v: "c->s[%d] = %s;" % (reg(0), v))
    out, T = "out%d" % w, "double" if w == 64 else "float"
    sign = "0x8000000000000000ull" if w == 64 else "0x80000000u"
    if op in ("vcmp", "vcmpe"):
        if len(ops) != 2 or not names[1]:
            raise Unsupported("instruction %s with zero" % op)
        return "fcmp(c, %s, %s);" % (get(0), get(1))
    if op == "vmov":
        return put(raw(1))
    if op == "vneg":  # a sign flip, no NaN or denormal handling (ARM's FPNeg)
        return put("%s ^ %s" % (raw(1), sign))
    if op == "vabs":
        return put("%s & ~%s" % (raw(1), sign))
    if op == "vsqrt":
        return put("%s(%s(%s))" % (out, "sqrt" if w == 64 else "sqrtf", get(1)))
    binop = {"vadd": "+", "vsub": "-", "vmul": "*", "vdiv": "/"}
    if op in binop:
        return put("%s((%s)(%s %s %s))" % (out, T, get(1), binop[op], get(2)))
    # the product rounded (and flushed, a NaN made the default one) before it is used: VFP's multiply-accumulate is not
    # fused; vnmul negates that product's bits (ARM's FPNeg after FPMul: a NaN product comes out as the negated default
    # NaN). The accumulator's input is flushed as any operand.
    prod = "%s((%s)(%s * %s))" % (out, T, get(1), get(2))
    if op == "vnmul":
        return put("%s ^ %s" % (prod, sign))
    if op in ("vmla", "vmls", "vnmla", "vnmls"):
        acc = "in%d(%s%s)" % (w, raw(0), " ^ " + sign if op in ("vnmla", "vnmls") else "")
        p = "in%d(%s%s)" % (w, prod, " ^ " + sign if op in ("vmls", "vnmla") else "")
        return put("%s((%s)(%s + %s))" % (out, T, acc, p))
    raise Unsupported("instruction %s" % i.mnemonic)


def translate(code, base, start, end, name, starts=frozenset(), callees=None, relocated=frozenset()):
    """the C of the function at start (to end at most), or Unsupported. starts: the function list, for calls and tail
    calls (b to a start); callees, when given, receives the functions it calls (they must be translated too);
    relocated: the words of the program the module loader rewrites (static_relocations), read from memory when run."""
    callees = set() if callees is None else callees
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    word = lambda a: struct.unpack_from("<I", code, a - base)[0]
    # the words inside the function that are data, never decoded: what its pc-relative loads read (literal pools,
    # between its code too), and the address tables of its switches (ldr pc, [pc, rN, lsl #2] and the words after it)
    # (only the loads of words that are code themselves count: a pool word read as an instruction can look like a
    # pc-relative ldrd and mark two real instructions as data, as in Omega Ruby's 004FBF20; repeated until it settles)
    data, switches = set(), {}
    for _ in range(16):
        new, sw = set(), {}
        for a in range(start, end, 4):
            if a in data:
                continue
            w = word(a)
            new.update(function_list.pc_relative_words(w, a))
            if w >> 28 != 0xF and (w & 0x0F7FF000) == 0x071FF000 and (w & 0xFF0) == 0x100:  # ldr pc, [pc, rN, lsl #2]
                table, b = [], a + 8
                while b < end and start <= word(b) < end and word(b) % 4 == 0:
                    table.append(word(b))
                    new.add(b)
                    b += 4
                sw[a] = table
        if new == data:
            break
        data, switches = new, sw
    else:
        raise Unsupported("its data words do not settle")
    insns = []
    for a in range(start, end, 4):
        if a in data:
            continue
        got = list(md.disasm(code[a - base:a - base + 4], a))
        if not got:
            break  # data after the function's last instruction
        insns.append(got[0])
    end_code = insns[-1].address + 4 if insns else start
    body, targets, returned = [], set(), False
    for i in insns:
        m = i.mnemonic
        base_m = m[:-2] if i.cc not in (0, 15) else m  # the condition suffix (bxne, addseq)
        base_m = base_m[:-1] if base_m.endswith("s") and i.update_flags else base_m
        ops = i.operands
        rd = lambda k=0: REG[i.reg_name(ops[k].reg)]
        line = []
        if m.startswith("v"):
            line.append(vfp(i, word, relocated))
        elif base_m == "nop":
            pass
        elif i.address in switches:  # ldr pc, [pc, rN, lsl #2]: a switch over the table after it
            table = switches[i.address]
            idx = REG[i.reg_name(ops[1].mem.index)]
            targets.update(table)
            line.append("switch (c->r[%d]) { %s default: c->fault = 5; return; }"
                        % (idx, " ".join("case %d: goto L%X;" % (k, t) for k, t in enumerate(table))))
        elif base_m == "bx" and i.reg_name(ops[0].reg) == "lr":
            # the C returns to its caller: right only when lr is the address this function got (a shared epilogue pops
            # lr from the stack: X's 00205E20); otherwise fault 3, never a silent other path
            line.append("if (c->r[14] != ret) { c->fault = 3; return; } return;")
            returned = i.cc in (0, 15)
        elif base_m == "bx" and ops[0].type == ARM_OP_REG:  # a jump through a register: the function it lands in returns
            line.append("{ uint32_t t = c->r[%d]; if (c->r[14] != ret) { c->fault = 3; return; } call_indirect(c, t, ret); "
                        "return; }" % rd())  # for this one (lr checked as at bx lr)
            returned = i.cc in (0, 15)
        elif base_m == "blx" and ops[0].type == ARM_OP_REG:  # an indirect call (virtual methods, function pointers)
            line.append("{ uint32_t t = c->r[%d]; c->r[14] = 0x%Xu; call_indirect(c, t, 0x%Xu); if (c->fault) return; }"
                        % (rd(), i.address + 4, i.address + 4))
        elif base_m == "bl":
            t = ops[0].imm
            if t not in starts or t % 4:
                raise Unsupported("call to a target that is no ARM function start")
            callees.add(t)
            line.append("c->r[14] = 0x%Xu; f_%08X(c, 0x%Xu); if (c->fault) return;" % (i.address + 4, t, i.address + 4))
        elif base_m == "b":
            t = ops[0].imm
            if start <= t < end_code:
                targets.add(t)
                line.append("goto L%X;" % t)
            elif t in starts and t % 4 == 0:  # a tail call: the callee returns for this function
                callees.add(t)
                # (it returns where this one would, to lr: so lr must be that address, as at bx lr)
                line.append("if (c->r[14] != ret) { c->fault = 3; return; } f_%08X(c, ret); return;" % t)
                returned = i.cc in (0, 15)
            else:
                raise Unsupported("branch out of the function to no function start")
        elif base_m in MULTI and not re.match(r"[sd]\d+$", i.reg_name(ops[0].reg)) or base_m in ("push", "pop"):
            mode, wb, load = MULTI[base_m]
            if base_m in ("push", "pop"):
                b, regs, wb = 13, [REG[i.reg_name(o.reg)] for o in ops], True
            else:
                b, regs, wb = REG[i.reg_name(ops[0].reg)], [REG[i.reg_name(o.reg)] for o in ops[1:]], i.writeback
            if any(re.match(r"[sd]\d+$", i.reg_name(o.reg)) for o in ops):
                raise Unsupported("instruction v%s" % base_m)
            if b == 15 or wb and b in regs or "^" in i.op_str or not load and 15 in regs or load and 13 in regs:
                raise Unsupported("ldm / stm form")
            n = len(regs)
            first = {"ia": "", "ib": " + 4u", "da": " - %du" % (4 * n - 4), "db": " - %du" % (4 * n)}[mode]
            line.append("{ uint32_t a = c->r[%d]%s;" % (b, first))
            for k, r in enumerate(sorted(regs)):
                if load:
                    line.append(" uint32_t v%d = ld32(c, a + %du);" % (k, 4 * k))
                else:
                    line.append(" st32(c, a + %du, c->r[%d]);" % (4 * k, r))
            line.append(" if (c->fault) return;")
            if wb:
                line.append(" c->r[%d] %s= %du;" % (b, "+" if mode in ("ia", "ib") else "-", 4 * n))
            if load:
                for k, r in enumerate(sorted(regs)):
                    line.append(" c->r[%d] = v%d;" % (r, k))
            line.append(" }")
            if load and 15 in regs:
                # the popped pc must be the return address this function got: a test whose stores overwrote it has no
                # meaning (the original would jump elsewhere); fault 3 makes the trial invalid
                line.append(" if (c->r[15] != ret) { c->fault = 3; return; } return;")
                returned = i.cc in (0, 15)
        elif base_m in ("lsl", "lsr", "asr", "ror", "rrx") and len(ops) == 2:  # capstone's alias of mov rd, rm, <shift>
            if rd() == 15:
                raise Unsupported("write to pc")
            if base_m == "rrx":  # capstone gives rrx with no shift on its operand: the carry in at the top, bit 0 out
                src = "c->r[%d]" % REG[i.reg_name(ops[1].reg)]
                e, carry = "((c->c << 31) | (%s >> 1))" % src, "(%s & 1)" % src
            else:
                e, carry = operand2(i, ops[1], i.update_flags)
            # the carry out read before rd is written (rd may be the shifted register: lsrs r0, r0, #8)
            line.append("{ uint32_t sk; uint32_t v = %s; uint32_t k = %s; c->r[%d] = v; (void)sk; (void)k;"
                        % (e, carry if carry else "c->c", rd()))
            if i.update_flags:
                line.append(" c->n = v >> 31; c->z = v == 0; c->c = k;")
            line.append(" }")
        elif base_m in ("lsl", "lsr", "asr", "ror") and len(ops) == 3 and ops[2].type == ARM_OP_REG:
            if rd() == 15 or REG[i.reg_name(ops[2].reg)] == 15:
                raise Unsupported("shift form")
            kind = {"lsl": 0, "lsr": 1, "asr": 2, "ror": 3}[base_m]
            line.append("{ uint32_t k, v = shreg(c, %d, c->r[%d], c->r[%d], &k); c->r[%d] = v;"
                        % (kind, REG[i.reg_name(ops[1].reg)], REG[i.reg_name(ops[2].reg)], rd()))
            if i.update_flags:
                line.append(" c->n = v >> 31; c->z = v == 0; c->c = k;")
            line.append(" (void)k; }")
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
            if rd() == 13:
                raise Unsupported("sp set from a register (a stack switch: 00123768 restores a saved context)")
            e, carry = operand2(i, ops[1], i.update_flags)
            v = e if base_m == "mov" else "~(%s)" % e
            line.append("{ uint32_t sk; uint32_t v = %s; uint32_t k = %s; c->r[%d] = v; (void)sk; (void)k;"
                        % (v, carry if carry else "c->c", rd()))  # (the carry read before rd is written)
            if i.update_flags:
                line.append(" c->n = v >> 31; c->z = v == 0; c->c = k;")
            line.append(" }")
        elif base_m in ("add", "sub", "rsb", "cmp", "cmn", "and", "orr", "eor", "bic", "tst", "teq"):
            compare = base_m in ("cmp", "cmn", "tst", "teq")
            logical = base_m in ("and", "orr", "eor", "bic", "tst", "teq")
            dst = None if compare else rd()
            if dst == 15:
                raise Unsupported("write to pc")
            if dst == 13 and not (base_m in ("add", "sub") and i.reg_name(ops[1].reg) == "sp"
                                  and ops[2].type == ARM_OP_IMM):
                raise Unsupported("sp set from a register (a stack switch: 00123768 restores a saved context)")
            a_op, b_op = (ops[0], ops[1]) if compare else (ops[1], ops[2] if len(ops) > 2 else None)
            if b_op is None:
                raise Unsupported("two-operand form")
            if i.reg_name(a_op.reg) == "pc":  # add / sub rX, pc, #k: an address, known here
                if base_m not in ("add", "sub") or compare or i.update_flags or b_op.type != ARM_OP_IMM:
                    raise Unsupported("pc-relative address")
                v = (i.address + 8 + (b_op.imm if base_m == "add" else -b_op.imm)) & 0xFFFFFFFF
                body.append((i.address, "    /* %08X %s %s */ %s" % (
                    i.address, m, i.op_str, ("if (%s) { c->r[%d] = 0x%Xu; }" % (COND[i.cc], dst, v)) if i.cc not in (0, 15)
                    else "c->r[%d] = 0x%Xu;" % (dst, v))))
                continue
            ra = "c->r[%d]" % REG[i.reg_name(a_op.reg)]
            flags = compare or i.update_flags
            e, carry = operand2(i, b_op, flags and logical)
            x, y = (e, ra) if base_m == "rsb" else (ra, e)
            line.append("{ uint32_t sk; uint32_t x = %s, y = %s, v; (void)sk;" % (x, y))
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
        elif base_m in ("uxtb", "uxth", "sxtb", "sxth") and len(ops) == 2:
            src = ops[1]
            if rd() == 15 or src.shift.type not in (0, 4):
                raise Unsupported("extend form")
            r = "c->r[%d]" % REG[i.reg_name(src.reg)]
            if src.shift.type == 4 and src.shift.value:  # ror #8 / #16 / #24 first
                n = src.shift.value
                r = "((%s >> %d) | (%s << %d))" % (r, n, r, 32 - n)
            cast = {"uxtb": "(uint32_t)(uint8_t)", "uxth": "(uint32_t)(uint16_t)", "sxtb": "(uint32_t)(int8_t)",
                    "sxth": "(uint32_t)(int16_t)"}[base_m]
            line.append("c->r[%d] = %s%s;" % (rd(), cast, r))
        elif base_m == "mul" and not i.update_flags:
            line.append("c->r[%d] = c->r[%d] * c->r[%d];" % (rd(), REG[i.reg_name(ops[1].reg)], REG[i.reg_name(ops[2].reg)]))
        elif base_m in ("ldrd", "strd"):  # two words: rt at the address, rt2 at + 4
            rt, rt2, mem = REG[i.reg_name(ops[0].reg)], REG[i.reg_name(ops[1].reg)], ops[2]
            basereg = i.reg_name(mem.mem.base)
            post = len(ops) > 3
            if mem.type != ARM_OP_MEM or 15 in (rt, rt2) or base_m == "ldrd" and 13 in (rt, rt2) or mem.mem.index or \
                    (i.writeback or post) and (basereg == "pc" or REG[basereg] in (rt, rt2)) or basereg == "pc":
                raise Unsupported("ldrd / strd form")
            b = "c->r[%d]" % REG[basereg]
            d = mem.mem.disp
            addr = b if post or not d else ("%s + 0x%Xu" % (b, d & 0xFFFFFFFF) if d > 0 else "%s - 0x%Xu" % (b, -d))
            line.append("{ uint32_t a = %s;" % addr)
            if base_m == "ldrd":
                line.append(" uint32_t v0 = ld32(c, a), v1 = ld32(c, a + 4u); if (c->fault) return;")
            else:
                line.append(" st32(c, a, c->r[%d]); st32(c, a + 4u, c->r[%d]); if (c->fault) return;" % (rt, rt2))
            if post:
                if ops[3].type != ARM_OP_IMM:
                    raise Unsupported("ldrd / strd form")
                k = ops[3].imm if not ops[3].subtracted else -ops[3].imm
                line.append(" %s = %s %s 0x%Xu;" % (b, b, "+" if k >= 0 else "-", abs(k)))
            elif i.writeback:
                line.append(" %s = a;" % b)
            if base_m == "ldrd":
                line.append(" c->r[%d] = v0; c->r[%d] = v1;" % (rt, rt2))
            line.append(" }")
        elif base_m in ("ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "str", "strb", "strh"):
            if base_m.startswith("ldr") and rd() == 13:
                raise Unsupported("sp set from a register (a stack switch: 00123768 restores a saved context)")
            mem = ops[1]
            if mem.type != ARM_OP_MEM:
                raise Unsupported("operand kind")
            basereg = i.reg_name(mem.mem.base)
            post = len(ops) > 2  # ldr rX, [rN], #off / rM: the access at rN, then rN moves
            if (i.writeback or post) and (basereg == "pc" or REG[basereg] == rd()):
                raise Unsupported("writeback form")
            if rd() == 15:
                if base_m == "ldr" and basereg == "sp" and post and ops[2].type == ARM_OP_IMM and i.cc in (0, 15):
                    # ldr pc, [sp], #4: pop {pc}, a return
                    line.append("{ uint32_t v = ld32(c, c->r[13]); if (c->fault) return; c->r[13] += 0x%Xu;"
                                " if (v != ret) { c->fault = 3; return; } } return;" % (ops[2].imm & 0xFFFFFFFF))
                    body.append((i.address, "    /* %08X %s %s */ %s" % (i.address, m, i.op_str, "".join(line))))
                    returned = True
                    if max(targets, default=0) <= i.address:
                        break
                    returned = False
                    continue
                raise Unsupported("load to pc")
            if basereg == "pc":
                if base_m != "ldr":
                    raise Unsupported("pc-relative non-word access")
                t = i.address + 8 + mem.mem.disp
                if t in relocated:  # a word the module loader rewrites (an import): its value only exists when run
                    line.append("c->r[%d] = ld32(c, 0x%Xu);" % (rd(), t))
                else:
                    line.append("c->r[%d] = 0x%Xu;" % (rd(), word(t)))
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
                newbase = None
                if post:  # the access at the base; the base moves by the third operand after it
                    off = ops[2]
                    if off.type == ARM_OP_IMM:
                        k = off.imm if not off.subtracted else -off.imm
                        step = ("+ 0x%Xu" % (k & 0xFFFFFFFF)) if k >= 0 else ("- 0x%Xu" % -k)
                    elif off.type == ARM_OP_REG and off.shift.type in (0, 2):
                        step = ("- " if off.subtracted else "+ ") + "c->r[%d]" % REG[i.reg_name(off.reg)]
                        if off.shift.type == 2 and off.shift.value:
                            step = step[:2] + "(%s << %d)" % (step[2:], off.shift.value)
                    else:
                        raise Unsupported("post-index form")
                    newbase = "c->r[%d] %s" % (REG[basereg], step)
                    addr = "c->r[%d]" % REG[basereg]
                elif i.writeback:  # pre-index with !: the base becomes the address
                    newbase = "a"
                load = {"ldr": "ld32(c, a)", "ldrb": "ld8(c, a)", "ldrh": "ld16(c, a)",
                        "ldrsb": "(uint32_t)(int8_t)ld8(c, a)", "ldrsh": "(uint32_t)(int16_t)ld16(c, a)"}
                line.append("{ uint32_t a = %s;" % addr)
                if base_m in load:
                    line.append(" uint32_t v = %s; if (c->fault) return;" % load[base_m])
                else:
                    line.append(" st%s(c, a, c->r[%d]); if (c->fault) return;"
                                % ({"str": "32", "strb": "8", "strh": "16"}[base_m], rd()))
                if newbase:
                    line.append(" c->r[%d] = %s;" % (REG[basereg], newbase))
                if base_m in load:
                    line.append(" c->r[%d] = v;" % rd())
                line.append(" }")
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
        last = insns[-1] if insns else None
        if last is not None and last.cc in (0, 15) and last.mnemonic == "b" and start <= last.operands[0].imm < end_code:
            pass  # it never returns: its last instruction branches back (a main loop); nothing falls off its end
        elif end_code in starts and end_code % 4 == 0 and not (last is not None and last.mnemonic == "b"):
            # it falls into the function placed after it (a tail call by placement: pop {r4, lr} then the next one)
            callees.add(end_code)
            body.append((end_code - 4, "    /* falls into %08X */ if (c->r[14] != ret) { c->fault = 3; return; } "
                                       "f_%08X(c, ret); return;" % (end_code, end_code)))
        else:
            raise Unsupported("no unconditional return found")
    # ret: the address this function returns to (its caller's bl + 4, or what a tail call passes on), for the pop {pc}
    # check
    out = ["void %s(Cpu* c, uint32_t ret)\n{\n    (void)ret;" % name]
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
