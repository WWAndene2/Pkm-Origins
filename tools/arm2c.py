#!/usr/bin/env python3
"""The translator, ARM to C, mechanically (steps 3 and 5 of docs/RECOMPILATION_PLAN.md §10): integer instructions, the
stack (push / pop, ldm / stm, writeback addressing) and direct calls and tail calls to other translated functions.
Each instruction becomes C on a register file and the flags; branches inside the function become gotos; literal pool
loads become constants. Anything outside this subset makes the function "unsupported" (returned as None, with the reason),
never translated approximately. Matched against the original by tools/match_functions.py.

  arm2c.py <code.bin> <address, hex> [--functions LIST] [--cro DIR] [--end ADDRESS]
      prints the C of one function (or why it is unsupported), and the functions it calls

The C uses: typedef struct { uint32_t r[16]; uint32_t n, z, c, v; int fault; uint32_t steps; uint32_t s[32], fpscr; uint32_t tls, mon, mon_addr, depth; } Cpu; and the memory helpers ld8/ld16/ld32,
st8/st16/st32 (tools/match_functions.py defines them: a fault flag instead of undefined behaviour out of the test memory).
"""
import argparse
import os
import re
import struct
import sys

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_OP_REG

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import function_list  # noqa: E402

REG = {"r0": 0, "r1": 1, "r2": 2, "r3": 3, "r4": 4, "r5": 5, "r6": 6, "r7": 7, "r8": 8, "sb": 9, "r9": 9, "sl": 10,
       "r10": 10, "fp": 11, "r11": 11, "ip": 12, "r12": 12, "sp": 13, "lr": 14, "pc": 15}
COND = {1: "c->z", 2: "!c->z", 3: "c->c", 4: "!c->c", 5: "c->n", 6: "!c->n", 7: "c->v", 8: "!c->v",
        9: "(c->c && !c->z)", 10: "(!c->c || c->z)", 11: "(c->n == c->v)", 12: "(c->n != c->v)",
        13: "(!c->z && c->n == c->v)", 14: "(c->z || c->n != c->v)"}
SHIFT = {1: "asr", 2: "lsl", 3: "lsr", 4: "ror"}


# the most bytes of one function the translators read (the game's longest: Omega Ruby's 001586A8, 20,172)
MAX_FUNCTION = 0x10000


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
/* the matching check's limits (HOS_MATCHING): a loop's steps (past 100,000: fault 2) and the call depth (past 2,000:
   fault 2, a random endless tail-call loop), so that a trial ends; the game itself runs with none */
#ifdef HOS_MATCHING
#define HOS_STEP(c) do { if (++(c)->steps > 100000) { (c)->fault = 2; return; } } while (0)
#define HOS_ENTER(c) do { if (++(c)->depth > 2000) { (c)->fault = 2; (c)->depth--; return; } } while (0)
#define HOS_LEAVE(c) ((c)->depth--)
#else
#define HOS_STEP(c) ((void)0)
#define HOS_ENTER(c) ((void)0)
#define HOS_LEAVE(c) ((void)0)
#endif
/* svc #n: the 3DS kernel's system call n (src/hos/: its arguments and results in r0-r7 as the kernel's ABI has them;
   the other registers kept) */
void hos_svc(Cpu* c, uint32_t n);
/* the host's rounding mode set to FPSCR's (bits 22-23: nearest, up, down, toward zero) */
#include <fenv.h>
static inline void hos_fpscr_mode(uint32_t fpscr)
{
    static const int mode[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    fesetround(mode[(fpscr >> 22) & 3]);
}
/* VFP, as ARM's VFPv2 computes, in the mode FPSCR gives: the 3DS kernel starts each thread with 0x03C00010 (Azahar's
   kernel/thread.cpp: default NaN, flush to zero, round toward zero) and the game's start sets 0x03000000 (round to
   nearest; vmsr below). The registers are bits (s[32]; d[k] is s[2k] low, s[2k+1] high). The arithmetic is the host's
   float / double in the host's rounding mode, which follows FPSCR's (each game thread is a host thread: vmsr sets it,
   the runtime sets each new thread's); the C is compiled with -frounding-math -ffp-contract=off (nothing folded or
   fused). These helpers add ARM's other rules: a denormal input is a zero of its sign; a result whose exact value is
   below the smallest normal number is a zero of its sign (ARM decides before rounding: the host may round such a
   result up to the smallest normal, which the rounding error, computed exactly, tells); a NaN result is the default
   NaN. */
static inline float F32(uint32_t x) { float f; memcpy(&f, &x, 4); return f; }
static inline double F64(uint64_t x) { double f; memcpy(&f, &x, 8); return f; }
static inline uint32_t fz32(uint32_t x) { return (x & 0x7F800000u) == 0 ? x & 0x80000000u : x; }
static inline uint64_t fz64(uint64_t x) { return (x & 0x7FF0000000000000ull) == 0 ? x & 0x8000000000000000ull : x; }
static inline float in32(uint32_t x) { return F32(fz32(x)); }
static inline double in64(uint64_t x) { return F64(fz64(x)); }
/* a result r: its bits, a zero of its sign when tiny (below the smallest normal) or when tiny is set (the exact value
   was, though r rounded up to the smallest normal), the default NaN for any NaN */
static inline uint32_t res32(float r, int tiny)
{
    uint32_t x;
    if (r != r) return 0x7FC00000u;
    memcpy(&x, &r, 4);
    return (x & 0x7FFFFFFFu) < 0x00800000u || tiny ? x & 0x80000000u : x;
}
static inline uint64_t res64(double r, int tiny)
{
    uint64_t x;
    if (r != r) return 0x7FF8000000000000ull;
    memcpy(&x, &r, 8);
    return (x & 0x7FFFFFFFFFFFFFFFull) < 0x0010000000000000ull || tiny ? x & 0x8000000000000000ull : x;
}
/* whether the exact product / quotient is below the smallest normal, computed exactly: for floats in double (a
   float product is exact there, and |a / b| < m is |a| < m |b|); for doubles on the mantissas (frexp), where fma
   gives the product's exact error and ldexp scales the quotient's bound exactly. A sum of normal numbers is tiny only
   by cancellation, and then exact (Sterbenz), so r tells; a square root of a normal number is never tiny. */
static inline int tiny_mul32(float a, float b) { double p = fabs((double)a * (double)b); return p != 0 && p < 0x1p-126; }
static inline int tiny_div32(float a, float b) { return a != 0 && fabs((double)a) < 0x1p-126 * fabs((double)b); }
static inline int tiny_mul64(double a, double b)
{
    if (a == 0 || b == 0 || a != a || b != b || isinf(a) || isinf(b)) return 0;
    int ea, eb;
    double ma = fabs(frexp(a, &ea)), mb = fabs(frexp(b, &eb)); /* in [0.5, 1) */
    double p = ma * mb, e = fma(ma, mb, -p);                     /* exact: ma mb = p + e */
    double t = ldexp(1.0, -1022 - ea - eb);                      /* the bound, scaled the same */
    return p < t || (p == t && e < 0);
}
static inline int tiny_div64(double a, double b)
{
    if (a == 0 || a != a || b != b || isinf(a) || b == 0 || isinf(b)) return 0;
    int ea, eb;
    double ma = fabs(frexp(a, &ea)), mb = fabs(frexp(b, &eb));
    return ma < ldexp(mb, -1022 - ea + eb); /* |a / b| < 2^-1022, both sides scaled by 2^-ea (exact: near 1) */
}
static inline uint32_t fadd32(float a, float b) { return res32(a + b, 0); }
static inline uint32_t fsub32(float a, float b) { return res32(a - b, 0); }
static inline uint32_t fmul32(float a, float b) { return res32(a * b, tiny_mul32(a, b)); }
static inline uint32_t fdiv32(float a, float b) { return res32(a / b, tiny_div32(a, b)); }
static inline uint32_t fsqrt32(float a) { return res32(sqrtf(a), 0); }
static inline uint64_t fadd64(double a, double b) { return res64(a + b, 0); }
static inline uint64_t fsub64(double a, double b) { return res64(a - b, 0); }
static inline uint64_t fmul64(double a, double b) { return res64(a * b, tiny_mul64(a, b)); }
static inline uint64_t fdiv64(double a, double b) { return res64(a / b, tiny_div64(a, b)); }
static inline uint64_t fsqrt64(double a) { return res64(sqrt(a), 0); }
/* conversions: double to float tiny when the exact double is below the smallest normal float; to double exact */
static inline uint32_t fcvt32(double x) { if (x != x) return 0x7FC00000u;
    return res32((float)x, x != 0 && fabs(x) < 0x1p-126); }
static inline uint64_t fcvt64(double x) { return res64(x, 0); }
static inline uint32_t out32(float f) { return res32(f, 0); }
static inline uint64_t out64(double f) { return res64(f, 0); }
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


# how the C writes an address of the code being translated: a constant for the .code (loaded where it was linked),
# B_<module> + its offset for a CRO module (loaded where the game's memory allocator puts it: its functions' addresses
# are known only when run; translate sets it)
ADDR = lambda x: "0x%Xu" % (x & 0xFFFFFFFF)


def fname(t, module=None):
    """the C name of the function at t: f_<address> in the .code, f_<module>_<offset> in a module"""
    return "f_%s_%06X" % (module, t) if module else "f_%08X" % t


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
    t, n = op.shift.type, op.shift.value
    if REG[md_insn.reg_name(op.reg)] == 15:  # pc reads as the instruction's address + 8 (Thumb, 16-bit: + 4)
        if t in SHIFT_REG:
            raise Unsupported("pc as an operand shifted by a register")
        r = ADDR(md_insn.address + (4 if md_insn.size == 2 else 8))
    if t == 0 or (t == 2 and n == 0):
        return r, None
    if t in SHIFT_REG:  # by a register: its low byte, with ARM's rules (the caller declares sk, the carry out)
        s = REG[md_insn.reg_name(n)]
        if s == 15:
            raise Unsupported("pc as an operand")
        return "shreg(c, %d, %s, c->r[%d], &sk)" % (SHIFT_REG[t], r, s), "sk"
    if t == 5:  # rrx: the carry in at the top, bit 0 out
        return "((c->c << 31) | (%s >> 1))" % r, "(%s & 1)" % r
    if n == 32 and t in (1, 3):  # lsr / asr #32 (encoded as #0): all out; the carry is bit 31
        return ("0u" if t == 3 else "(uint32_t)((int32_t)%s >> 31)" % r), "(%s >> 31)" % r
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


ARITH = {"mla", "mls", "umull", "smull", "umlal", "smlal", "mlas", "umulls", "smulls",
         "smulbb", "smulbt", "smultb", "smultt", "smulwb", "smulwt", "smlabb", "smlabt", "smlatb", "smlatt", "smlawb",
         "smlawt", "smlalbb", "smlalbt", "smlaltb", "smlaltt", "clz", "rev", "rev16", "revsh", "pld", "usat", "ssat",
         "qadd", "qsub", "uxtab", "uxtah", "sxtab", "sxtah", "pkhbt", "pkhtb", "uqsub8", "uqsub16", "uqadd8", "uqadd16",
         "smlad", "smladx", "smuad", "smuadx", "uxtb16", "uxtab16", "qdadd", "qdsub",
         "qadd16", "qsub16", "smusd", "smusdx", "usat16", "uhadd8",
         "ssat16", "qasx", "qsax", "sadd16", "ssub16", "sasx", "ssax"}


def arith(i, op, flags):
    """the multiplies, the saturating and packing instructions, clz, rev and pld. The sticky overflow flag Q (set by
    qadd, ssat, smla<x><y> on overflow) is not kept: nothing reads it but mrs, which is refused."""
    op = op[:-1] if op in ("mlas", "umulls", "smulls") else op
    ops = i.operands
    regs = [REG[i.reg_name(o.reg)] if o.type == ARM_OP_REG else None for o in ops]
    if op == "pld":  # a cache hint: no access, no fault
        return "/* pld */"
    if 15 in [r for r in regs if r is not None] or 13 in regs[:1]:
        raise Unsupported("instruction %s form" % op)
    R = lambda k: "c->r[%d]" % regs[k]
    nz = " c->n = v >> 31; c->z = v == 0;" if flags else ""
    half = lambda r, top: "(int32_t)(int16_t)(%s >> 16)" % r if top else "(int32_t)(int16_t)%s" % r
    if op in ("qadd16", "qsub16"):  # each signed halfword, saturated
        lanes = []
        for k in (0, 16):
            e = "((int32_t)(int16_t)(%s >> %d) %s (int32_t)(int16_t)(%s >> %d))" % (R(1), k, "+" if op == "qadd16" else "-",
                                                                                 R(2), k)
            lanes.append("(((uint32_t)(%s > 32767 ? 32767 : %s < -32768 ? -32768 : %s) & 0xFFFFu) << %d)" % (e, e, e, k))
        return "c->r[%d] = %s;" % (regs[0], " | ".join(lanes))
    if op in ("qasx", "qsax", "sasx", "ssax", "sadd16", "ssub16"):
        # per signed halfword: (low, high) of rn with rm's (asx: high + low, low - high; sax: the other way), saturated
        # for q; the GE flags the s forms set are not kept: only sel reads them, and neither game has one
        lo_n, hi_n = "(int32_t)(int16_t)%s" % R(1), "(int32_t)(int16_t)(%s >> 16)" % R(1)
        lo_m, hi_m = "(int32_t)(int16_t)%s" % R(2), "(int32_t)(int16_t)(%s >> 16)" % R(2)
        lo, hi = {"qasx": ("%s - %s" % (lo_n, hi_m), "%s + %s" % (hi_n, lo_m)),
                  "sasx": ("%s - %s" % (lo_n, hi_m), "%s + %s" % (hi_n, lo_m)),
                  "qsax": ("%s + %s" % (lo_n, hi_m), "%s - %s" % (hi_n, lo_m)),
                  "ssax": ("%s + %s" % (lo_n, hi_m), "%s - %s" % (hi_n, lo_m)),
                  "sadd16": ("%s + %s" % (lo_n, lo_m), "%s + %s" % (hi_n, hi_m)),
                  "ssub16": ("%s - %s" % (lo_n, lo_m), "%s - %s" % (hi_n, hi_m))}[op]
        sat = (lambda e: "((%s) > 32767 ? 32767 : (%s) < -32768 ? -32768 : (%s))" % (e, e, e)) if op[0] == "q" else \
            (lambda e: "(%s)" % e)
        return "c->r[%d] = ((uint32_t)%s & 0xFFFFu) | (((uint32_t)%s & 0xFFFFu) << 16);" % (regs[0], sat(lo), sat(hi))
    if op == "ssat16":  # each signed halfword clamped to [-2^(n-1), 2^(n-1) - 1]
        n = ops[1].imm
        lo_, hi_ = -(1 << (n - 1)), (1 << (n - 1)) - 1
        lanes = ["(((uint32_t)(%s < %d ? %d : %s > %d ? %d : %s) & 0xFFFFu) << %d)" % (e, lo_, lo_, e, hi_, hi_, e, k)
                 for k, e in ((0, "(int32_t)(int16_t)%s" % R(2)), (16, "(int32_t)(int16_t)(%s >> 16)" % R(2)))]
        return "c->r[%d] = %s;" % (regs[0], " | ".join(lanes))
    if op in ("smusd", "smusdx"):  # low product minus high product
        m_ = "((%s >> 16) | (%s << 16))" % (R(2), R(2)) if op.endswith("x") else R(2)
        return "{ uint32_t m_ = %s; c->r[%d] = (uint32_t)(%s * %s - %s * %s); }" % (
            m_, regs[0], half(R(1), False), half("m_", False), half(R(1), True), half("m_", True))
    if op == "usat16":  # each signed halfword clamped to [0, 2^n - 1]
        n = ops[1].imm
        hi = (1 << n) - 1
        lanes = ["(((uint32_t)(%s < 0 ? 0 : %s > %d ? %d : %s)) << %d)" % ((e,) * 2 + (hi, hi, e, k)) for k, e in
                 ((0, "(int32_t)(int16_t)%s" % R(2)), (16, "(int32_t)(int16_t)(%s >> 16)" % R(2)))]
        return "c->r[%d] = %s;" % (regs[0], " | ".join(lanes))
    if op == "uhadd8":  # each byte: (a + b) / 2
        lanes = ["((((%s >> %d) & 0xFFu) + ((%s >> %d) & 0xFFu)) >> 1 << %d)" % (R(1), k, R(2), k, k) for k in (0, 8, 16, 24)]
        return "c->r[%d] = %s;" % (regs[0], " | ".join(lanes))
    if op in ("smlad", "smladx", "smuad", "smuadx"):  # two signed halfword products added (x: rm's halves swapped)
        m_ = "((%s >> 16) | (%s << 16))" % (R(2), R(2)) if op.endswith("x") else R(2)
        acc = " + %s" % R(3) if op.startswith("smla") else ""
        return "{ uint32_t m_ = %s; c->r[%d] = (uint32_t)(%s * %s + %s * %s)%s; }" % (
            m_, regs[0], half(R(1), False), half("m_", False), half(R(1), True), half("m_", True), acc)
    if op in ("uxtb16", "uxtab16"):
        src = ops[-1]
        r = R(len(ops) - 1)
        if src.shift.type == 4 and src.shift.value:
            r = "((%s >> %d) | (%s << %d))" % (r, src.shift.value, r, 32 - src.shift.value)
        elif src.shift.type:
            raise Unsupported("instruction %s form" % op)
        if op == "uxtb16":
            return "c->r[%d] = %s & 0x00FF00FFu;" % (regs[0], r)
        return ("{ uint32_t x = %s, n_ = %s; c->r[%d] = ((n_ + (x & 0xFFu)) & 0xFFFFu) | ((((n_ >> 16) + ((x >> 16) & 0xFFu))"
                " & 0xFFFFu) << 16); }" % (r, R(1), regs[0]))
    if op in ("qdadd", "qdsub"):  # rd = sat(rm +- sat(2 * rn))
        sat = lambda e: "(%s > 2147483647LL ? 2147483647LL : %s < -2147483648LL ? -2147483648LL : %s)" % (e, e, e)
        return "{ int64_t d = %s; int64_t x = (int64_t)(int32_t)%s %s d; c->r[%d] = (uint32_t)(int32_t)%s; }" % (
            sat("2 * (int64_t)(int32_t)%s" % R(2)), R(1), "+" if op == "qdadd" else "-", regs[0], sat("x"))
    if op in ("mla", "mls"):
        return "{ uint32_t v = %s %s %s * %s; c->r[%d] = v;%s }" % (R(3), "+" if op == "mla" else "-", R(1), R(2),
                                                                  regs[0], nz)
    if op in ("umull", "smull", "umlal", "smlal"):
        signed = op[0] == "s"
        prod = "(uint64_t)((int64_t)(int32_t)%s * (int32_t)%s)" % (R(2), R(3)) if signed else \
            "(uint64_t)%s * %s" % (R(2), R(3))
        acc = " + ((uint64_t)%s << 32 | %s)" % (R(1), R(0)) if op.endswith("lal") else ""
        f = " c->n = (uint32_t)(v >> 63); c->z = v == 0;" if flags else ""
        return "{ uint64_t v = %s%s; c->r[%d] = (uint32_t)v; c->r[%d] = (uint32_t)(v >> 32);%s }" % (
            prod, acc, regs[0], regs[1], f)
    if op.startswith("smul") and op[4] in "bt":
        return "c->r[%d] = (uint32_t)(%s * %s);" % (regs[0], half(R(1), op[4] == "t"), half(R(2), op[5] == "t"))
    if op.startswith("smulw"):
        return "c->r[%d] = (uint32_t)(int32_t)(((int64_t)(int32_t)%s * %s) >> 16);" % (regs[0], R(1), half(R(2), op[5] == "t"))
    if op.startswith("smlaw"):
        return "c->r[%d] = (uint32_t)(int32_t)(((int64_t)(int32_t)%s * %s) >> 16) + %s;" % (
            regs[0], R(1), half(R(2), op[5] == "t"), R(3))
    if op.startswith("smlal"):
        return ("{ uint64_t v = ((uint64_t)%s << 32 | %s) + (uint64_t)(int64_t)(%s * %s); c->r[%d] = (uint32_t)v;"
                " c->r[%d] = (uint32_t)(v >> 32); }" % (R(1), R(0), half(R(2), op[5] == "t"), half(R(3), op[6] == "t"),
                                                       regs[0], regs[1]))
    if op.startswith("smla"):
        return "c->r[%d] = (uint32_t)(%s * %s) + %s;" % (regs[0], half(R(1), op[4] == "t"), half(R(2), op[5] == "t"), R(3))
    if op == "clz":
        return "c->r[%d] = %s ? (uint32_t)__builtin_clz(%s) : 32u;" % (regs[0], R(1), R(1))
    if op == "rev":
        return "c->r[%d] = __builtin_bswap32(%s);" % (regs[0], R(1))
    if op == "rev16":
        return "{ uint32_t x = %s; c->r[%d] = ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu); }" % (R(1), regs[0])
    if op == "revsh":
        return "c->r[%d] = (uint32_t)(int32_t)(int16_t)(((%s & 0xFFu) << 8) | ((%s >> 8) & 0xFFu));" % (regs[0], R(1), R(1))
    if op in ("usat", "ssat"):
        n, src = ops[1].imm, ops[2]
        x = "(int64_t)(int32_t)%s" % R(2)
        if src.shift.type == 2 and src.shift.value:
            x = "(int64_t)(int32_t)(%s << %d)" % (R(2), src.shift.value)
        elif src.shift.type == 1 and src.shift.value:
            x = "(int64_t)((int32_t)%s >> %d)" % (R(2), min(src.shift.value, 31))
        elif src.shift.type not in (0, 2):
            raise Unsupported("instruction %s form" % op)
        lo, hi = (0, (1 << n) - 1) if op == "usat" else (-(1 << (n - 1)), (1 << (n - 1)) - 1)
        return "{ int64_t x = %s; c->r[%d] = (uint32_t)(int32_t)(x < %dLL ? %dLL : x > %dLL ? %dLL : x); }" % (
            x, regs[0], lo, lo, hi, hi)
    if op in ("qadd", "qsub"):
        return ("{ int64_t x = (int64_t)(int32_t)%s %s (int64_t)(int32_t)%s; c->r[%d] = (uint32_t)(int32_t)(x > 2147483647LL"
                " ? 2147483647LL : x < -2147483648LL ? -2147483648LL : x); }" % (R(1), "+" if op == "qadd" else "-", R(2),
                                                                               regs[0]))
    if op in ("uxtab", "uxtah", "sxtab", "sxtah"):
        src = ops[2]
        r = R(2)
        if src.shift.type == 4 and src.shift.value:
            r = "((%s >> %d) | (%s << %d))" % (r, src.shift.value, r, 32 - src.shift.value)
        elif src.shift.type not in (0,):
            raise Unsupported("instruction %s form" % op)
        cast = {"uxtab": "(uint32_t)(uint8_t)", "uxtah": "(uint32_t)(uint16_t)", "sxtab": "(uint32_t)(int8_t)",
                "sxtah": "(uint32_t)(int16_t)"}[op]
        return "c->r[%d] = %s + %s%s;" % (regs[0], R(1), cast, r)
    if op in ("pkhbt", "pkhtb"):
        src = ops[2]
        n = src.shift.value if len(ops) > 2 else 0
        if op == "pkhbt":
            if src.shift.type not in (0, 2):
                raise Unsupported("instruction %s form" % op)
            return "c->r[%d] = (%s & 0xFFFFu) | ((%s << %d) & 0xFFFF0000u);" % (regs[0], R(1), R(2), n)
        if src.shift.type != 1:
            raise Unsupported("instruction %s form" % op)
        return "c->r[%d] = (%s & 0xFFFF0000u) | ((uint32_t)((int32_t)%s >> %d) & 0xFFFFu);" % (regs[0], R(1), R(2), min(n, 31))
    if op in ("uqsub8", "uqsub16", "uqadd8", "uqadd16"):
        bits = 8 if op.endswith("8") else 16
        mask = (1 << bits) - 1
        sub = "sub" in op
        lanes = []
        for k in range(0, 32, bits):
            a_, b_ = "((%s >> %d) & 0x%Xu)" % (R(1), k, mask), "((%s >> %d) & 0x%Xu)" % (R(2), k, mask)
            e = "(%s > %s ? %s - %s : 0u)" % (a_, b_, a_, b_) if sub else \
                "(%s + %s > 0x%Xu ? 0x%Xu : %s + %s)" % (a_, b_, mask, mask, a_, b_)
            lanes.append("(%s << %d)" % (e, k))
        return "c->r[%d] = %s;" % (regs[0], " | ".join(lanes))
    raise Unsupported("instruction %s" % op)


EXCLUSIVE = {"ldrex": 32, "ldrexh": 16, "ldrexb": 8, "ldrexd": 64, "strex": 32, "strexh": 16, "strexb": 8, "strexd": 64}


def coprocessor(i, op):
    """mrc / mcr: the two forms the game uses. mrc p15, 0, rd, c13, c0, 3 reads the thread's user read-only ID register,
    which the 3DS kernel sets to the thread's local storage (TLS: the IPC command buffer at + 0x80); the C reads c->tls,
    which the runtime sets for each thread (the trials: a place in the test memory). mcr p15, 0, rX, c7, c10, 4 / 5 are
    ARMv6's data synchronization and memory barriers: one thread's C needs none."""
    f = i.op_str.replace(" ", "").split(",")
    if op == "mrc" and f[0] == "p15" and f[1] == "#0" and f[3:] == ["c13", "c0", "#3"] and f[2] in REG and REG[f[2]] < 13:
        return "c->r[%d] = c->tls;" % REG[f[2]]
    if op == "mcr" and f[0] == "p15" and f[1] == "#0" and f[3:5] == ["c7", "c10"] and f[5] in ("#4", "#5"):
        return "/* barrier */"
    raise Unsupported("instruction %s %s" % (op, "p15" if f[0] == "p15" else "other coprocessor"))


def exclusive(i, op):
    """ldrex / strex (and their byte, halfword and doubleword forms) and clrex, on one CPU's exclusive monitor: ldrex
    opens it on its address (c->mon = 1, c->mon_addr), strex stores and writes 0 to its status register only while it
    is open on that address, else writes 1 and stores nothing; both strex and clrex close it. (Another thread's store
    cannot happen inside one translated function: the runtime's threads are its concern.)"""
    if op == "clrex":
        return "c->mon = 0;"
    ops = i.operands
    regs = [REG[i.reg_name(o.reg)] for o in ops if o.type == ARM_OP_REG]
    mem = ops[-1]
    if mem.type != ARM_OP_MEM or mem.mem.disp or mem.mem.index or 15 in regs or 13 in regs:
        raise Unsupported("instruction %s form" % op)
    b = REG[i.reg_name(mem.mem.base)]
    size = EXCLUSIVE[op]
    ld = {32: "ld32", 16: "ld16", 8: "ld8"}
    st = {32: "st32", 16: "st16", 8: "st8"}
    if op.startswith("ldrex"):
        if size == 64:
            return ("{ uint32_t a = c->r[%d]; if (a %% 8) { c->fault = 1; return; } uint32_t v0 = ld32(c, a), v1 = ld32(c, a + 4u);"
                    " if (c->fault) return; c->r[%d] = v0; c->r[%d] = v1; c->mon = 1; c->mon_addr = a; }" % (b, regs[0], regs[1]))
        return ("{ uint32_t a = c->r[%d]; uint32_t v = %s(c, a); if (c->fault) return; c->r[%d] = v; c->mon = 1;"
                " c->mon_addr = a; }" % (b, ld[size], regs[0]))
    status = regs[0]
    if status == b or status in regs[1:-1] if size == 64 else status == b or status == regs[1]:
        raise Unsupported("instruction %s form" % op)
    if size == 64:
        store = "if (a %% 8) { c->fault = 1; return; } st32(c, a, c->r[%d]); st32(c, a + 4u, c->r[%d]);" % (regs[1], regs[2])
    else:
        store = "%s(c, a, c->r[%d]);" % (st[size], regs[1])
    return ("{ uint32_t a = c->r[%d]; if (c->mon && c->mon_addr == a) { %s if (c->fault) return; c->r[%d] = 0; }"
            " else c->r[%d] = 1; c->mon = 0; }" % (b, store, status, status))


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
                words.append("ld32(c, %s)" % ADDR(x) if x in relocated else "0x%Xu" % word(x))
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
            fs, fd = (lambda n: re.match(r"s\d+$", n)), (lambda n: re.match(r"d\d+$", n))  # (sb, sl, sp are core)
            if fs(a) and fs(b):
                return "c->s[%d] = c->s[%d];" % (sreg(0), sreg(1))
            if fd(a) and fd(b):
                return "wr64(c, %d, rd64(c, %d));" % (dreg(0), dreg(1))
            if fs(a):
                return "c->s[%d] = c->r[%d];" % (sreg(0), core(1))
            if fs(b):
                return "c->r[%d] = c->s[%d];" % (core(0), sreg(1))
        if len(ops) == 3 and names[2] and re.match(r"d\d+$", names[2]):  # vmov rt, rt2, dm
            return "{ uint32_t lo = c->s[%d], hi = c->s[%d]; c->r[%d] = lo; c->r[%d] = hi; }" % (
                2 * dreg(2), 2 * dreg(2) + 1, core(0), core(1))
        if len(ops) == 3 and names[0] and re.match(r"d\d+$", names[0]):  # vmov dm, rt, rt2
            return "c->s[%d] = c->r[%d]; c->s[%d] = c->r[%d];" % (2 * dreg(0), core(1), 2 * dreg(0) + 1, core(2))
        raise Unsupported("instruction vmov form")
    if op == "vmsr" and names[0] == "fpscr":
        # FPSCR written: its rounding mode becomes the host's (hos_fpscr_mode); flush to zero and default NaN must stay
        # on (what the C does), else the thread stops (fault 10) rather than compute differently
        return ("{ uint32_t x = c->r[%d]; if ((x & 0x03000000u) != 0x03000000u || (x & 0x00300000u)) { c->r[15] = %s;"
                " c->fault = 10; return; } c->fpscr = x; hos_fpscr_mode(x); }" % (core(1), ADDR(i.address)))
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
            return "wr64(c, %d, fcvt64((double)%s));" % (dst, x)
        if to == "f32":  # (from f64: tiny below the float range; from an integer: rounded in the FPSCR mode)
            return "c->s[%d] = %s;" % (dst, "fcvt32(%s)" % x if frm == "f64" else "out32((float)%s)" % x)
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
        return put("fsqrt%d(%s)" % (w, get(1)))
    binop = {"vadd": "fadd", "vsub": "fsub", "vmul": "fmul", "vdiv": "fdiv"}
    if op in binop:
        return put("%s%d(%s, %s)" % (binop[op], w, get(1), get(2)))
    # the product rounded (and flushed, a NaN made the default one) before it is used: VFP's multiply-accumulate is not
    # fused; vnmul negates that product's bits (ARM's FPNeg after FPMul: a NaN product comes out as the negated default
    # NaN). The accumulator's input is flushed as any operand.
    prod = "fmul%d(%s, %s)" % (w, get(1), get(2))
    if op == "vnmul":
        return put("%s ^ %s" % (prod, sign))
    if op in ("vmla", "vmls", "vnmla", "vnmls"):
        acc = "in%d(%s%s)" % (w, raw(0), " ^ " + sign if op in ("vnmla", "vnmls") else "")
        p = "in%d(%s%s)" % (w, prod, " ^ " + sign if op in ("vmls", "vnmla") else "")
        return put("fadd%d(%s, %s)" % (w, acc, p))
    raise Unsupported("instruction %s" % i.mnemonic)


def translate(code, base, start, end, name, starts=frozenset(), callees=None, relocated=frozenset(), thumb=False,
              thumb_starts=frozenset(), entries=None, module=None):
    """the C of the function at start (to end at most), or Unsupported. starts: the ARM functions of the list, for calls
    and tail calls (b to a start), thumb_starts the Thumb ones; thumb: the function itself is Thumb (Thumb-1 and the
    bl / blx pair, as ARMv6K has it); callees, when given, receives the functions it calls (they must be translated
    too); entries, when given, receives the targets of branches into the middle of other functions, taken as tail
    calls to entries of their own (else such a branch is refused); module: the CRO module the code is from (code is then
    the module's file, base 0, and the C's addresses B_<module> + offset: see ADDR); relocated: the words of the program the module loader rewrites (static_relocations), read from memory when run.
    In Thumb pc reads as the instruction's address + 4 (word-aligned for pc-relative loads and adr), and a call's lr has
    bit 0 set: the C passes the same values on, so returns and their checks work across the two instruction sets."""
    global ADDR
    ADDR = (lambda x: "0x%Xu" % (x & 0xFFFFFFFF)) if not module else \
        (lambda x, m=module: "(B_%s + 0x%Xu)" % (m, x & 0xFFFFFFFF))
    callees = set() if callees is None else callees
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
    md.detail = True
    word = lambda a: struct.unpack_from("<I", code, a - base)[0]
    half = lambda a: struct.unpack_from("<H", code, a - base)[0]
    pc_of = (lambda a: (a + 4) & ~3) if thumb else (lambda a: a + 8)  # pc as an address operand (adr, literal loads)

    def is_start(t, to_thumb):
        return t in thumb_starts if to_thumb else t in starts and t % 4 == 0
    # the words inside the function that are data, never decoded: what its pc-relative loads read (literal pools,
    # between its code too), and the address tables of its switches (ldr pc, [pc, rN, lsl #2] and the words after it)
    # (only the loads of words that are code themselves count: a pool word read as an instruction can look like a
    # pc-relative ldrd and mark two real instructions as data, as in Omega Ruby's 004FBF20; repeated until it settles)
    data, switches = set(), {}
    if thumb:  # the literal pools its ldr rX, [pc, #imm] read: their two halfwords are data
        for a in range(start, end, 2):
            h = half(a)
            if h >> 11 == 0x09:
                t = pc_of(a) + (h & 0xFF) * 4
                if start <= t < end:
                    data.update((t, t + 2))
    for _ in range(0 if thumb else 16):
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
        if not thumb:
            raise Unsupported("its data words do not settle")
    insns = []
    a = start
    while a < end:
        if a in data:
            a += 2 if thumb else 4
            continue
        got = list(md.disasm(code[a - base:a - base + 4], a))
        if not got:  # a word no instruction encodes: data (text between a function's pieces, or after its end)
            a += 2 if thumb else 4
            continue
        if thumb and (got[0].size == 4 and got[0].mnemonic not in ("bl", "blx") or got[0].mnemonic in ("cbz", "cbnz", "it")
                      or got[0].mnemonic.startswith("it")):
            raise Unsupported("Thumb-2 instruction (not on ARMv6K)")
        insns.append(got[0])
        a += got[0].size
    end_code = insns[-1].address + insns[-1].size if insns else start
    computed = []

    def jump(expr):
        """a computed jump (add pc, ... / mov pc, rN / ldr pc, ...): to any instruction of this function by a switch
        on the target, else out of it as a tail call through call_indirect"""
        if thumb:
            raise Unsupported("computed jump in Thumb")
        computed.append(True)
        # (the cases: every instruction translated, filled in once the function is: code past its last return, which
        # only such a jump could reach, is not decoded, as data may follow; a jump there is a tail call through
        # call_indirect, which faults (4) when nothing is translated there)
        return ("{ uint32_t t = %s; switch (t - %s) { @CASES@ default: break; } call_indirect(c, t, ret); return; }" % (expr, ADDR(0)))
    body, targets, returned = [], set(), False
    unsupported = []  # the instructions with no C (a stop each)
    for i in insns:
        m = i.mnemonic
        base_m = m[:-2] if i.cc not in (0, 15) else m  # the condition suffix (bxne, addseq)
        base_m = base_m[:-1] if base_m.endswith("s") and i.update_flags else base_m
        ops = i.operands
        rd = lambda k=0: REG[i.reg_name(ops[k].reg)]
        line = []
        try:
            if m.startswith("v"):
                line.append(vfp(i, word, relocated))
            elif base_m == "svc":
                line.append("hos_svc(c, 0x%Xu); if (c->fault) return;" % (ops[0].imm & 0xFFFFFF))
            elif base_m == "mrs" and i.op_str.replace(" ", "") in ("%s,apsr" % i.reg_name(ops[0].reg), "%s,cpsr" % i.reg_name(ops[0].reg)):
                # the flags read as a word: N Z C V in bits 31-28; the mode bits of user mode (0x10); Q and GE, which the C
                # does not keep, read as 0 (a game that set them would show it in the matching: Unicorn keeps them)
                line.append("c->r[%d] = c->n << 31 | c->z << 30 | c->c << 29 | c->v << 28 | 0x10u;" % rd())
            elif base_m == "msr" and i.op_str.replace(" ", "").split(",")[0] in ("apsr_nzcvq", "apsr_nzcv", "cpsr_f") \
                    and ops[-1].type == ARM_OP_REG:
                # the flags written from a word (bits 31-28); Q, which the C does not keep, dropped
                line.append("{ uint32_t x = c->r[%d]; c->n = x >> 31; c->z = (x >> 30) & 1; c->c = (x >> 29) & 1;"
                            " c->v = (x >> 28) & 1; }" % REG[i.reg_name(ops[-1].reg)])
            elif base_m in ARITH:
                line.append(arith(i, base_m, i.cc in (0, 15) and m.endswith("s") or i.cc not in (0, 15) and m[:-2].endswith("s")))
            elif base_m in ("mrc", "mcr"):
                line.append(coprocessor(i, base_m))
            elif base_m in EXCLUSIVE or base_m == "clrex":
                line.append(exclusive(i, base_m))
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
                line.append("{ uint32_t t = c->r[%d]; call_indirect(c, t, ret); "
                            "return; }" % rd())  # for this one
                returned = i.cc in (0, 15)
            elif base_m == "blx" and ops[0].type == ARM_OP_REG:  # an indirect call (virtual methods, function pointers)
                lr = (i.address + i.size) | thumb
                line.append("{ uint32_t t = c->r[%d]; c->r[14] = %s; call_indirect(c, t, %s); if (c->fault) return; }"
                            % (rd(), ADDR(lr), ADDR(lr)))
            elif base_m in ("bl", "blx") and ops[0].type == ARM_OP_IMM:  # blx #imm: a call that switches instruction set
                t = ops[0].imm & 0xFFFFFFFF
                to_thumb = thumb != (base_m == "blx")
                if to_thumb is False:
                    t &= ~3  # (a Thumb blx's target is word-aligned)
                if not is_start(t, to_thumb):
                    raise Unsupported("call to a target that is no function start")
                callees.add(t)
                lr = (i.address + i.size) | thumb
                line.append("c->r[14] = %s; %s(c, %s); if (c->fault) return;" % (ADDR(lr), fname(t, module), ADDR(lr)))
            elif base_m == "b":
                t = ops[0].imm
                if start <= t < end_code:
                    targets.add(t)
                    line.append("goto L%X;" % t)
                elif is_start(t, thumb) or entries is not None:  # a tail call: the callee returns for this function
                    # (a target that is no start is inside another function: code the compiler shares between functions,
                    # one function's tail jumped into from others; it is translated as its own entry, from there to the
                    # end of the function holding it, by the caller of translate: entries receives it)
                    if not is_start(t, thumb):
                        entries.add(t)
                    callees.add(t)
                    # (a jump, not a call: lr is whatever it holds, often the return address of the last call made, as
                    # when a function jumps to an epilogue it shares; the return address is checked where the code
                    # actually returns, at its bx lr or pop {pc})
                    line.append("%s(c, ret); return;" % fname(t, module))
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
                # (a store with writeback whose base is in the list stores the base's old value when it is the lowest
                # register listed, ARM's one defined case: the C stores before it writes the base back)
                stm_base_lowest = not load and wb and b in regs and b == min(regs)
                if b == 15 or wb and b in regs and not stm_base_lowest or "^" in i.op_str or not load and 15 in regs \
                        or load and 13 in regs:
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
            elif thumb and base_m in ("lsl", "lsr", "asr", "ror") and len(ops) == 2 and ops[1].type == ARM_OP_REG:
                # Thumb's lsls rd, rs: rd shifted by rs
                kind = {"lsl": 0, "lsr": 1, "asr": 2, "ror": 3}[base_m]
                line.append("{ uint32_t k, v = shreg(c, %d, c->r[%d], c->r[%d], &k); c->r[%d] = v;"
                            % (kind, rd(), REG[i.reg_name(ops[1].reg)], rd()))
                if i.update_flags:
                    line.append(" c->n = v >> 31; c->z = v == 0; c->c = k;")
                line.append(" (void)k; }")
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
                r = "c->r[%d]" % REG[i.reg_name(ops[1].reg)]
                if rd() != 15 and n == 32 and base_m in ("lsr", "asr"):  # lsrs / asrs #32: all out, the carry is bit 31
                    line.append("{ uint32_t k = %s >> 31, v = %s; c->r[%d] = v;" % (
                        r, "0u" if base_m == "lsr" else "(uint32_t)((int32_t)%s >> 31)" % r, rd()))
                    if i.update_flags:
                        line.append(" c->n = v >> 31; c->z = v == 0; c->c = k;")
                    line.append(" (void)k; }")
                elif rd() == 15 or not 0 < n < 32:
                    raise Unsupported("shift form")
                else:
                    e = {"lsl": "(%s << %d)" % (r, n), "lsr": "(%s >> %d)" % (r, n),
                         "asr": "(uint32_t)((int32_t)%s >> %d)" % (r, n), "ror": "((%s >> %d) | (%s << (32 - %d)))" % (r, n, r, n)}[base_m]
                    carry = "((%s >> %d) & 1)" % (r, 32 - n) if base_m == "lsl" else "((%s >> %d) & 1)" % (r, n - 1)
                    line.append("{ uint32_t k = %s, v = %s; c->r[%d] = v;" % (carry, e, rd()))
                    if i.update_flags:
                        line.append(" c->n = v >> 31; c->z = v == 0; c->c = k;")
                    line.append(" (void)k; }")
            elif base_m in ("mov", "mvn") and rd() == 15:
                if base_m == "mov" and ops[1].type == ARM_OP_REG and i.reg_name(ops[1].reg) == "lr" and not ops[1].shift.type \
                        and not i.update_flags:  # mov pc, lr: a return (ARMv4's)
                    line.append("if (c->r[14] != ret) { c->fault = 3; return; } return;")
                elif i.update_flags:
                    raise Unsupported("write to pc with flags (an exception return)")
                else:
                    e, _ = operand2(i, ops[1], False)
                    line.append("{ uint32_t sk; (void)sk; %s }" % jump(e if base_m == "mov" else "~(%s)" % e))
                returned = i.cc in (0, 15)
            elif base_m in ("mov", "mvn"):
                # (sp set from a register, as anything else: in the C sp is a register pointing into the emulated memory; a
                # stack switched for good shows at the return, whose address is checked)
                e, carry = operand2(i, ops[1], i.update_flags)
                v = e if base_m == "mov" else "~(%s)" % e
                line.append("{ uint32_t sk; uint32_t v = %s; uint32_t k = %s; c->r[%d] = v; (void)sk; (void)k;"
                            % (v, carry if carry else "c->c", rd()))  # (the carry read before rd is written)
                if i.update_flags:
                    line.append(" c->n = v >> 31; c->z = v == 0; c->c = k;")
                line.append(" }")
            elif base_m == "adr":  # an address, known here
                if rd() == 15:
                    raise Unsupported("write to pc")
                line.append("c->r[%d] = %s;" % (rd(), ADDR(pc_of(i.address) + ops[1].imm)))
            elif base_m in ("add", "sub", "rsb", "adc", "sbc", "rsc", "cmp", "cmn", "and", "orr", "eor", "bic", "tst", "teq"):
                compare = base_m in ("cmp", "cmn", "tst", "teq")
                if not compare and len(ops) == 2:  # Thumb's two-operand form: adds rd, rm / add sp, #k is rd = rd op ...
                    ops = [ops[0], ops[0], ops[1]]
                logical = base_m in ("and", "orr", "eor", "bic", "tst", "teq")
                dst = None if compare else rd()
                to_pc = dst == 15
                if to_pc:  # a computed jump (a table of cases: add pc, r5, r6), its target computed as rd would be
                    if (m[:-2] if i.cc not in (0, 15) else m).endswith("s") or thumb:
                        raise Unsupported("write to pc with flags (an exception return)" if not thumb else "write to pc")
                    dst = 16  # (a scratch: c->r[16] does not exist; the value goes to the jump below)
                a_op, b_op = (ops[0], ops[1]) if compare else (ops[1], ops[2] if len(ops) > 2 else None)
                if b_op is None:
                    raise Unsupported("two-operand form")
                pc_first = i.reg_name(a_op.reg) == "pc"
                if pc_first and base_m in ("add", "sub") and not compare and not i.update_flags and b_op.type == ARM_OP_IMM \
                        and not to_pc:  # add / sub rX, pc, #k: an address, known here
                    v = (pc_of(i.address) + (b_op.imm if base_m == "add" else -b_op.imm)) & 0xFFFFFFFF
                    body.append((i.address, "    /* %08X %s %s */ %s" % (
                        i.address, m, i.op_str, ("if (%s) { c->r[%d] = %s; }" % (COND[i.cc], dst, ADDR(v))) if i.cc not in (0, 15)
                        else "c->r[%d] = %s;" % (dst, ADDR(v)))))
                    continue
                # (pc as the first operand otherwise: its value, the instruction's address + 8, or + 4 in Thumb)
                ra = ADDR(i.address + (4 if thumb else 8)) if pc_first else "c->r[%d]" % REG[i.reg_name(a_op.reg)]
                # (from the mnemonic: capstone reports update_flags for adc / sbc with no s, as they read the carry)
                flags = compare or (m[:-2] if i.cc not in (0, 15) else m).endswith("s")
                e, carry = operand2(i, b_op, flags and logical)
                x, y = (e, ra) if base_m in ("rsb", "rsc") else (ra, e)
                line.append("{ uint32_t sk; uint32_t x = %s, y = %s, v; (void)sk;" % (x, y))
                if base_m in ("add", "cmn"):
                    line.append(" v = x + y;")
                    if flags:
                        line.append(" c->c = v < x; c->v = ((~(x ^ y) & (x ^ v)) >> 31);")
                elif base_m == "adc":
                    line.append(" uint64_t w = (uint64_t)x + y + c->c; v = (uint32_t)w;")
                    if flags:
                        line.append(" c->c = (uint32_t)(w >> 32); c->v = ((~(x ^ y) & (x ^ v)) >> 31);")
                elif base_m in ("sbc", "rsc"):  # x - y - not carry
                    line.append(" uint64_t w = (uint64_t)x + (uint32_t)~y + c->c; v = (uint32_t)w;")
                    if flags:
                        line.append(" c->c = (uint32_t)(w >> 32); c->v = (((x ^ y) & (x ^ v)) >> 31);")
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
                if to_pc:
                    line.append(" %s" % jump("v"))
                    returned = i.cc in (0, 15)
                elif not compare:
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
            elif base_m == "mul":  # (muls sets N and Z; on ARMv6 C and V are kept)
                src = [REG[i.reg_name(o.reg)] for o in ops[1:]] if len(ops) == 3 else [rd(), REG[i.reg_name(ops[1].reg)]]
                line.append("{ uint32_t v = c->r[%d] * c->r[%d]; c->r[%d] = v;" % (src[0], src[1], rd()))
                if i.update_flags:
                    line.append(" c->n = v >> 31; c->z = v == 0;")
                line.append(" }")
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
                    if base_m == "ldr" and not post and not i.writeback and (basereg != "pc" or mem.mem.index):
                        # ldr pc, [rN, ...] / [pc, rM, lsl #2] (a table of cases whose words the module loader writes,
                        # so not a constant table): the target read from memory where the CPU reads it, then jumped to
                        sh, n = mem.shift.type, mem.shift.value
                        if sh not in (0, 2) or mem.subtracted:
                            raise Unsupported("load to pc")
                        b = ADDR(pc_of(i.address)) if basereg == "pc" else "c->r[%d]" % REG[basereg]
                        if mem.mem.index:
                            off = "(c->r[%d] << %d)" % (REG[i.reg_name(mem.mem.index)], n) if sh == 2 else \
                                "c->r[%d]" % REG[i.reg_name(mem.mem.index)]
                        else:
                            off = "0x%Xu" % (mem.mem.disp & 0xFFFFFFFF)
                        line.append("{ uint32_t tv = ld32(c, %s + %s); if (c->fault) return; %s }" % (b, off, jump("tv")))
                    elif basereg == "pc" and not post and not i.writeback and base_m == "ldr":
                        # ldr pc, [pc, #-4]: a veneer jumping to the word after it (an import the module loader writes)
                        t = pc_of(i.address) + mem.mem.disp
                        line.append(jump("ld32(c, %s)" % ADDR(t) if t in relocated else "0x%Xu" % word(t)))
                    else:
                        raise Unsupported("load to pc")
                    line.append(" if (c->fault) return;")
                    stmt = "".join(line)
                    body.append((i.address, "    /* %08X %s %s */ %s" % (i.address, m, i.op_str, (
                        "if (%s) { %s }" % (COND[i.cc], stmt)) if i.cc not in (0, 15) else stmt)))
                    returned = i.cc in (0, 15)
                    if returned and max(targets, default=0) <= i.address:
                        break
                    returned = False
                    continue
                if basereg == "pc":
                    if base_m != "ldr":
                        raise Unsupported("pc-relative non-word access")
                    t = pc_of(i.address) + mem.mem.disp
                    if t in relocated:  # a word the module loader rewrites (an import): its value only exists when run
                        line.append("c->r[%d] = ld32(c, %s);" % (rd(), ADDR(t)))
                    else:
                        line.append("c->r[%d] = 0x%Xu;" % (rd(), word(t)))
                else:
                    addr = "c->r[%d]" % REG[basereg]
                    if mem.mem.index:
                        idx = "c->r[%d]" % REG[i.reg_name(mem.mem.index)]
                        sh, n = mem.shift.type, mem.shift.value
                        if sh == 2:
                            idx = "(%s << %d)" % (idx, n)
                        elif sh == 3 and 0 < n < 32:
                            idx = "(%s >> %d)" % (idx, n)
                        elif sh == 1 and 0 < n < 32:
                            idx = "(uint32_t)((int32_t)%s >> %d)" % (idx, n)
                        elif sh == 4 and 0 < n < 32:
                            idx = "((%s >> %d) | (%s << %d))" % (idx, n, idx, 32 - n)
                        elif sh != 0:
                            raise Unsupported("shifted index")
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
        except Unsupported as e:
            # an instruction the translator has no C for: the thread stops there (fault 10, pc its address), the
            # function still translated; data the walk took for code after a call that does not return is the usual
            # case (Omega Ruby's 0017E2E8, after bl to abort). Reached, it stops: nothing is guessed.
            unsupported.append((i.address, str(e)))
            line = ["c->r[15] = %s; c->fault = 10; return; /* UNSUPPORTED: %s */" % (ADDR(i.address), str(e).replace("*/", "* /"))]
            returned = i.cc in (0, 15)
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
        elif is_start(end_code, thumb) and not (last is not None and last.mnemonic == "b"):
            # it falls into the function placed after it (a tail call by placement: pop {r4, lr} then the next one)
            callees.add(end_code)
            body.append((last.address, "    /* falls into %08X */ "
                                       "%s(c, ret); return;" % (end_code, fname(end_code, module))))
        elif last is not None and last.cc in (0, 15) and last.mnemonic in ("bl", "blx"):
            # its last instruction calls a function that does not return (abort, a fatal error): should it return, the
            # original would run whatever follows; the C stops on fault 8 instead of guessing
            body.append((last.address, "    /* the call above does not return */ c->fault = 8; return;"))
        else:
            # its code ends with no return the walk could see (data after its last instruction, or a conditional one
            # last): should the thread get there, it stops (fault 10 at that address) rather than run on
            body.append((last.address if last is not None else start, "    /* no return past here */ c->r[15] = %s;"
                         " c->fault = 10; return;" % ADDR(end_code)))
            unsupported.append((end_code, "no return found"))
    # ret: the address this function returns to (its caller's bl + 4, or what a tail call passes on), for the pop {pc}
    # check
    # the function's body, and the function: a wrapper counting how deep the calls go (tail calls and calls through
    # pointers can loop without end on a test's random values: past 2,000 the trial stops on fault 2, as a long loop
    # does, instead of overflowing the C's stack)
    out = ["static void %s_body(Cpu* c, uint32_t ret)\n{\n    (void)ret;" % name]
    if computed:
        done = sorted({a for a, _ in body if a < end_code})
        cases = " ".join("case 0x%Xu: goto L%X;" % (x, x) for x in done)
        body = [(a, s.replace("@CASES@", cases)) for a, s in body]
        targets.update(done)
    # (a return-address fault records where it happened: r15 the instruction's address)
    body = [(a, s.replace("c->fault = 3;", "c->r[15] = %s; c->fault = 3;" % ADDR(a))) for a, s in body]
    labelled = set()
    for a, s in body:
        if a in targets and a not in labelled:  # (once: a line added after an instruction carries its address too)
            labelled.add(a)
            out.append("L%X: HOS_STEP(c);" % a)
        out.append(s)
    # a branch to a place with no instruction translated (data there, or past where decoding stopped): the thread
    # stops there (fault 10), so the C is whole
    for t in sorted(targets - labelled):
        out.append("L%X: c->r[15] = %s; c->fault = 10; return; /* UNSUPPORTED: a branch to no translated code */"
                   % (t, ADDR(t)))
    out.append("}")
    out.append("void %s(Cpu* c, uint32_t ret)\n{\n    HOS_ENTER(c);\n    %s_body(c, ret);\n    HOS_LEAVE(c);\n}\n"
               % (name, name))
    return "\n".join(out), 4 * len(body)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("code")
    p.add_argument("address", type=lambda s: int(s, 16))
    p.add_argument("--functions", help="the function list (symbols/*_functions.tsv): the function's size and instruction "
                   "set, and the starts its calls and tail calls must reach, as match_functions.py translates it")
    p.add_argument("--end", type=lambda s: int(s, 16))
    p.add_argument("--base", type=lambda s: int(s, 0), default=0x100000)
    p.add_argument("--cro", help="the game's modules: the words they rewrite are read when run")
    a = p.parse_args()
    code = open(a.code, "rb").read()
    arm, thumb, size = frozenset(), frozenset(), {}
    if a.functions:
        rows = [l.split() for l in open(a.functions) if l.strip() and not l.startswith("#")]
        thumb = frozenset(int(r[0], 16) for r in rows if r[3] == "thumb")
        arm = frozenset(int(r[0], 16) for r in rows if r[3] != "thumb")
        size = {int(r[0], 16): int(r[4]) for r in rows}
    end = a.end or a.address + min(size.get(a.address) or 0x400, MAX_FUNCTION)
    callees = set()
    try:
        print(translate(code, a.base, a.address, end, "f_%08X" % a.address, arm, callees,
                        static_relocations(a.cro) if a.cro else frozenset(), a.address in thumb, thumb)[0])
        if callees:
            print("/* calls: %s */" % " ".join("%08X" % t for t in sorted(callees)))
    except Unsupported as e:
        print("unsupported:", e)


if __name__ == "__main__":
    main()
