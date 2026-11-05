/* The PICA200 shader translator to GLSL: see PicaShaderGlsl.h. Each case below writes what PicaShader.c does for the
 * same instruction; the decoding is the same (bits and names as there). */
#include "PicaShaderGlsl.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define BITS(w, at, n) (((w) >> (at)) & ((1u << (n)) - 1))

typedef struct { char* at; size_t size, used; int full; } Out;

static void put(Out* o, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->at + o->used, o->used < o->size ? o->size - o->used : 0, fmt, ap);
    va_end(ap);
    if (n < 0 || o->used + n >= o->size)
        o->full = 1;
    else
        o->used += n;
}

/* a source register as a GLSL vec4 expression (an address-register index read at run time) */
static void source(Out* o, uint32_t reg, uint32_t address_index, char* buf, size_t n)
{
    if (reg < 0x10)
        snprintf(buf, n, "pica_v[%u]", reg);
    else if (reg < 0x20)
        snprintf(buf, n, "r[%u]", reg - 0x10);
    else if (address_index)
        snprintf(buf, n, "cu(%u, a[%u])", reg - 0x20, address_index - 1);
    else
        snprintf(buf, n, "pica_c[%u]", reg - 0x20);
    (void)o;
}

/* the swizzled, maybe negated operand: vec4(R.x, R.y, R.z, R.w) */
static void operand(Out* o, const char* name, const char* reg, uint32_t selector, int negate)
{
    static const char comp[] = "xyzw";
    put(o, "    precise vec4 %s = %svec4(%s.%c, %s.%c, %s.%c, %s.%c);\n", name, negate ? "-" : "",
        reg, comp[BITS(selector, 6, 2)], reg, comp[BITS(selector, 4, 2)], reg, comp[BITS(selector, 2, 2)],
        reg, comp[BITS(selector, 0, 2)]);
}

static void destination(uint32_t reg, char* buf, size_t n)
{
    if (reg < 0x10)
        snprintf(buf, n, "pica_o[%u]", reg);
    else if (reg < 0x20)
        snprintf(buf, n, "r[%u]", reg - 0x10);
    else
        snprintf(buf, n, "scratch");
}

/* d.<k> = <expr with k> for each written component */
static void each(Out* o, const char* d, uint32_t mask, const char* fmt)
{
    static const char comp[] = "xyzw";
    for (int k = 0; k < 4; k++)
        if (mask & (8u >> k)) {
            char e[256];
            snprintf(e, sizeof e, fmt, comp[k], comp[k], comp[k], comp[k]);
            put(o, "    { precise float t = %s; %s.%c = t; }\n", e, d, comp[k]); /* (precise: kept as written) */
        }
}

static void condition(Out* o, uint32_t w)
{
    const char* x = BITS(w, 25, 1) ? "cc.x" : "!cc.x";
    const char* y = BITS(w, 24, 1) ? "cc.y" : "!cc.y";
    switch (BITS(w, 22, 2)) {
    case 0: put(o, "(%s || %s)", x, y); break;
    case 1: put(o, "(%s && %s)", x, y); break;
    case 2: put(o, "(%s)", x); break;
    default: put(o, "(%s)", y); break;
    }
}

static const char prologue[] =
    /* NaN tested on the bits: a GPU compiler may take isnan() as always false (as lavapipe did) */
    "bool nan24(float x) { return (floatBitsToUint(x) & 0x7FFFFFFFu) > 0x7F800000u; }\n"
    "float m24(float a, float b) { precise float p = a * b; return (nan24(p) && !nan24(a) && !nan24(b)) ? 0.0 : p; }\n"
    /* a sum whose two terms are zeros: its sign worked out on the bits (-0 only from two -0), as a compiler may fold
       0.0 + y into y across m24's select and keep y's -0 */
    "float a24(float x, float y) { precise float s = x + y;\n"
    "    return (x == 0.0 && y == 0.0) ? uintBitsToFloat(floatBitsToUint(x) & floatBitsToUint(y) & 0x80000000u) : s; }\n"
    "vec4 cu(uint index, int offset) {\n"
    "    if (offset < -128 || offset > 127) offset = 0;\n"
    "    uint i = (index + uint(offset)) & 0x7Fu;\n"
    "    return i >= 96u ? vec4(1.0) : pica_c[i];\n"
    "}\n"
    "int to_s32(float v) { return (v >= -2147483648.0 && v < 2147483648.0) ? int(v) : int(0x80000000u); }\n"
    "void pica_main() {\n"
    "    precise vec4 r[16];\n"
    "    for (int k = 0; k < 16; k++) r[k] = vec4(0.0);\n"
    "    precise vec4 scratch = vec4(0.0);\n"
    "    int a[3] = int[3](0, 0, 0);\n"
    "    bvec2 cc = bvec2(false);\n"
    "    uint if_else[8], if_end[8]; int nif = 0;\n"
    "    uint call_end[4], call_ret[4]; int ncall = 0;\n"
    "    uint loop_entry[4], loop_end[4], loop_down[4], loop_step[4], loop_prev[4]; int nloop = 0;\n"
    "    uint pc = %uu;\n"
    "    for (uint steps = 0u; steps < %uu; steps++) {\n"
    "        bool brk = false;\n"
    "        uint old_pc = pc;\n"
    "        switch (pc) {\n";

/* the epilogue of every instruction: PicaShader.c's stack pops, in its order */
static const char epilogue[] =
    "        }\n"
    "        pc++;\n"
    "        uint next = old_pc + 1u;\n"
    "        for (int k = 0; k < 4; k++) {\n"
    "            if (ncall == 0 || call_end[ncall - 1] != next) break;\n"
    "            if (k < 3) { pc = call_ret[ncall - 1]; next = pc; }\n"
    "            ncall--;\n"
    "        }\n"
    "        if (nif > 0 && if_else[nif - 1] == old_pc + 1u) { pc = if_end[nif - 1]; nif--; }\n"
    "        if (nloop > 0 && (loop_end[nloop - 1] == old_pc + 1u || brk)) {\n"
    "            a[2] += int(loop_step[nloop - 1]);\n"
    "            uint down = loop_down[nloop - 1];\n"
    "            loop_down[nloop - 1] = (down - 1u) & 0xFFu;\n"
    "            if (!brk && down != 0u) pc = loop_entry[nloop - 1];\n"
    "            else { pc = loop_end[nloop - 1]; if (nloop > 1) a[2] = int(loop_prev[nloop - 1]); nloop--; }\n"
    "        }\n"
    "    }\n"
    "}\n";

/* a push onto a stack of cap entries, the oldest dropped when full (boost::circular_buffer) */
static void push(Out* o, const char* n, int cap, const char* const* names, const char* const* values, int fields)
{
    put(o, "    if (%s == %d) { for (int k = 0; k < %d; k++) {", n, cap, cap - 1);
    for (int f = 0; f < fields; f++)
        put(o, " %s[k] = %s[k + 1];", names[f], names[f]);
    put(o, " } %s--; }\n", n);
    for (int f = 0; f < fields; f++)
        put(o, "    %s[%s] = %s;\n", names[f], n, values[f]);
    put(o, "    %s++;\n", n);
}

long pica_shader_glsl(const PicaShaderSetup* s, uint32_t entry, uint32_t max_steps, char* out, size_t size)
{
    Out o = {out, size, 0, 0};
    uint32_t words = PICA_PROGRAM_WORDS - 1;
    while (words > 0 && !s->code[words - 1])
        words--;
    put(&o, prologue, entry, max_steps);
    for (uint32_t pc = 0; pc < words; pc++) {
        const uint32_t w = s->code[pc];
        uint32_t op = w >> 26;
        if ((op & ~7u) == 0x38) op = 0x38;
        else if ((op & ~7u) == 0x30) op = 0x30;
        else if ((op & ~1u) == 0x2E) op = 0x2E;
        put(&o, "        case %uu: {\n", pc);
        char r1[64], r2[64], r3[64], d[64];
        if (op < 0x20 || op == 0x2E) {
            const uint32_t desc = s->swizzle[BITS(w, 0, 7)];
            const int inv = op == 0x18 || op == 0x19 || op == 0x1A || op == 0x1B;
            const uint32_t ar = BITS(w, 19, 2);
            source(&o, inv ? BITS(w, 14, 5) : BITS(w, 12, 7), inv ? 0 : ar, r1, sizeof r1);
            source(&o, inv ? BITS(w, 7, 7) : BITS(w, 7, 5), inv ? ar : 0, r2, sizeof r2);
            operand(&o, "s1", r1, BITS(desc, 5, 8), BITS(desc, 4, 1));
            operand(&o, "s2", r2, BITS(desc, 14, 8), BITS(desc, 13, 1));
            destination(BITS(w, 21, 5), d, sizeof d);
            const uint32_t mask = BITS(desc, 0, 4);
            switch (op) {
            case 0x00: each(&o, d, mask, "a24(s1.%c, s2.%c)"); break;
            case 0x08: each(&o, d, mask, "m24(s1.%c, s2.%c)"); break;
            case 0x0B: each(&o, d, mask, "floor(s1.%c)"); break;
            case 0x0C: each(&o, d, mask, "(s1.%c > s2.%c) ? s1.%c : s2.%c"); break;
            case 0x0D: each(&o, d, mask, "(s1.%c < s2.%c) ? s1.%c : s2.%c"); break;
            case 0x01: case 0x02: case 0x03: case 0x18:
                if (op == 0x03 || op == 0x18)
                    put(&o, "    s1.w = 1.0;\n");
                /* (0 + p0 written out: a GPU compiler folds a sum with 0.0 into p0, keeping a -0 that the
                   interpreter's sum makes +0) */
                put(&o, "    precise float dot = m24(s1.x, s2.x);\n    if (dot == 0.0) dot = 0.0;\n");
                for (int k = 1; k < (op == 0x01 ? 3 : 4); k++)
                    put(&o, "    dot = a24(dot, m24(s1.%c, s2.%c));\n", "xyzw"[k], "xyzw"[k]);
                each(&o, d, mask, "dot");
                break;
            case 0x0E: put(&o, "    precise float res = 1.0 / s1.x;\n"); each(&o, d, mask, "res"); break;
            case 0x0F: put(&o, "    precise float res = inversesqrt(s1.x);\n"); each(&o, d, mask, "res"); break;
            case 0x05: put(&o, "    precise float res = exp2(s1.x);\n"); each(&o, d, mask, "res"); break;
            case 0x06: put(&o, "    precise float res = log2(s1.x);\n"); each(&o, d, mask, "res"); break;
            case 0x12:
                for (int k = 0; k < 2; k++)
                    if (mask & (8u >> k))
                        put(&o, "    a[%d] = to_s32(s1.%c);\n", k, "xy"[k]);
                break;
            case 0x13: each(&o, d, mask, "s1.%c"); break;
            case 0x09: case 0x1A: each(&o, d, mask, "(s1.%c >= s2.%c) ? 1.0 : 0.0"); break;
            case 0x0A: case 0x1B: each(&o, d, mask, "(s1.%c < s2.%c) ? 1.0 : 0.0"); break;
            case 0x2E: {
                static const char* ops[] = {"==", "!=", "<", "<=", ">", ">="};
                for (int k = 0; k < 2; k++) {
                    uint32_t c = BITS(w, k == 0 ? 24 : 21, 3);
                    if (c < 6)
                        put(&o, "    cc.%c = s1.%c %s s2.%c;\n", "xy"[k], "xy"[k], ops[c], "xy"[k]);
                }
                break;
            }
            default: break;
            }
        } else if (op == 0x38 || op == 0x30) {
            const uint32_t desc = s->swizzle[BITS(w, 0, 5)];
            const int inv = op == 0x30;
            const uint32_t ar = BITS(w, 22, 2);
            source(&o, BITS(w, 17, 5), 0, r1, sizeof r1);
            source(&o, inv ? BITS(w, 12, 5) : BITS(w, 10, 7), inv ? 0 : ar, r2, sizeof r2);
            source(&o, inv ? BITS(w, 5, 7) : BITS(w, 5, 5), inv ? ar : 0, r3, sizeof r3);
            operand(&o, "s1", r1, BITS(desc, 5, 8), BITS(desc, 4, 1));
            operand(&o, "s2", r2, BITS(desc, 14, 8), BITS(desc, 13, 1));
            operand(&o, "s3", r3, BITS(desc, 23, 8), BITS(desc, 22, 1));
            destination(BITS(w, 24, 5), d, sizeof d);
            each(&o, d, BITS(desc, 0, 4), "a24(m24(s1.%c, s2.%c), s3.%c)");
        } else {
            const uint32_t count = BITS(w, 0, 8), target = BITS(w, 10, 12);
            const uint32_t bool_id = BITS(w, 22, 4), int_id = BITS(w, 22, 2);
            char v1[64], v2[64], v3[64], v4[64], v5[64];
            switch (op) {
            case 0x22: put(&o, "    return;\n"); break;
            case 0x2C: put(&o, "    if "); condition(&o, w); put(&o, " pc = %uu;\n", target - 1); break;
            case 0x2D: put(&o, "    if (pica_b[%u] == %d) pc = %uu;\n", bool_id, !(count & 1), target - 1); break;
            case 0x24: case 0x25: case 0x26: {
                if (op == 0x25) { put(&o, "    if "); condition(&o, w); put(&o, " {\n"); }
                else if (op == 0x26) put(&o, "    if (pica_b[%u] != 0) {\n", bool_id);
                else put(&o, "    {\n");
                snprintf(v1, sizeof v1, "%uu", target + count);
                snprintf(v2, sizeof v2, "%uu", pc + 1);
                const char* names[] = {"call_end", "call_ret"};
                const char* values[] = {v1, v2};
                push(&o, "ncall", 4, names, values, 2);
                put(&o, "    pc = %uu;\n    }\n", target - 1);
                break;
            }
            case 0x27: case 0x28: {
                if (op == 0x27) put(&o, "    if (pica_b[%u] != 0) {\n", bool_id);
                else { put(&o, "    if "); condition(&o, w); put(&o, " {\n"); }
                snprintf(v1, sizeof v1, "%uu", target);
                snprintf(v2, sizeof v2, "%uu", target + count);
                const char* names[] = {"if_else", "if_end"};
                const char* values[] = {v1, v2};
                push(&o, "nif", 8, names, values, 2);
                put(&o, "    } else pc = %uu;\n", target - 1);
                break;
            }
            case 0x29: {
                put(&o, "    a[2] = pica_i[%u].y;\n", int_id);
                snprintf(v1, sizeof v1, "%uu", pc + 1);
                snprintf(v2, sizeof v2, "%uu", target + 1);
                snprintf(v3, sizeof v3, "uint(pica_i[%u].x) & 0xFFu", int_id);
                snprintf(v4, sizeof v4, "uint(pica_i[%u].z) & 0xFFu", int_id);
                snprintf(v5, sizeof v5, "uint(a[2]) & 0xFFu");
                const char* names[] = {"loop_entry", "loop_end", "loop_down", "loop_step", "loop_prev"};
                const char* values[] = {v1, v2, v3, v4, v5};
                push(&o, "nloop", 4, names, values, 5);
                break;
            }
            case 0x20: put(&o, "    brk = true;\n"); break;
            case 0x23: put(&o, "    if "); condition(&o, w); put(&o, " brk = true;\n"); break;
            default: break;
            }
        }
        put(&o, "        } break;\n");
    }
    put(&o, "        default: return;\n");
    put(&o, epilogue);
    return o.full ? -1 : (long)o.used;
}
