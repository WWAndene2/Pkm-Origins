/* The PICA200 shader interpreter: see PicaShader.h. Instruction encodings as nihstro (Azahar's externals) and 3dbrew
 * give them: opcode in bits 26-31; arithmetic: operand descriptor 0-6, src2 7-11 (inverted forms: 7-13), src1 12-18
 * (inverted: 14-18), address register 19-20, dest 21-25; mad: descriptor 0-4, src3 5-9 (inverted 5-11), src2 10-16
 * (inverted 12-16), src1 17-21, address register 22-23, dest 24-28; flow control: count 0-7, target 10-21, condition
 * op / uniform 22-25, refy 24, refx 25. Operand descriptor: dest mask 0-3 (bit 3 = x), negations at 4, 13, 22, the
 * selectors of src1 5-12, src2 14-21, src3 23-30 (x's in the top two bits of each). */
#include "PicaShader.h"

#include <math.h>
#include <string.h>

enum {
    OP_ADD = 0x00, OP_DP3 = 0x01, OP_DP4 = 0x02, OP_DPH = 0x03, OP_DST = 0x04, OP_EX2 = 0x05, OP_LG2 = 0x06,
    OP_LIT = 0x07, OP_MUL = 0x08, OP_SGE = 0x09, OP_SLT = 0x0A, OP_FLR = 0x0B, OP_MAX = 0x0C, OP_MIN = 0x0D,
    OP_RCP = 0x0E, OP_RSQ = 0x0F, OP_MOVA = 0x12, OP_MOV = 0x13, OP_DPHI = 0x18, OP_DSTI = 0x19, OP_SGEI = 0x1A,
    OP_SLTI = 0x1B, OP_BREAK = 0x20, OP_NOP = 0x21, OP_END = 0x22, OP_BREAKC = 0x23, OP_CALL = 0x24, OP_CALLC = 0x25,
    OP_CALLU = 0x26, OP_IFU = 0x27, OP_IFC = 0x28, OP_LOOP = 0x29, OP_EMIT = 0x2A, OP_SETEMIT = 0x2B, OP_JMPC = 0x2C,
    OP_JMPU = 0x2D, OP_CMP = 0x2E, OP_MADI = 0x30, OP_MAD = 0x38
};

#define BITS(w, at, n) (((w) >> (at)) & ((1u << (n)) - 1))

static const float ones[4] = {1, 1, 1, 1};

/* Azahar's float24 product: a NaN from two non-NaN factors (0 x infinity) is 0 */
static float mul24(float a, float b)
{
    float r = a * b;
    if (isnan(r) && !isnan(a) && !isnan(b))
        r = 0;
    return r;
}

/* static_cast<s32>(float) as x86's cvttss2si gives it (Azahar's host): out of range or NaN -> INT32_MIN */
static int32_t to_s32(float v)
{
    if (!(v >= -2147483648.0f && v < 2147483648.0f))
        return INT32_MIN;
    return (int32_t)v;
}

static const float* source(const PicaShaderSetup* s, const PicaShaderUnit* u, uint32_t reg, uint32_t address_index)
{
    if (reg < 0x10)
        return u->input[reg];
    if (reg < 0x20)
        return u->temp[reg - 0x10];
    uint32_t index = reg - 0x20;
    if (address_index) {
        int32_t offset = u->address[address_index - 1];
        if (offset < -128 || offset > 127)
            offset = 0;
        index = (index + offset) & 0x7F;
        if (index >= 96)
            return ones;
    }
    return s->f[index];
}

/* the four components a descriptor's selector picks, negated if asked */
static void pick(const float* reg, uint32_t selector, int negate, float out[4])
{
    for (int k = 0; k < 4; k++) {
        out[k] = reg[BITS(selector, 6 - 2 * k, 2)];
        if (negate)
            out[k] = -out[k];
    }
}

static float* destination(PicaShaderUnit* u, uint32_t reg, float scratch[4])
{
    if (reg < 0x10)
        return u->output[reg];
    if (reg < 0x20)
        return u->temp[reg - 0x10];
    return scratch;
}

static int condition(const PicaShaderUnit* u, uint32_t w)
{
    int x = BITS(w, 25, 1) == u->cond[0], y = BITS(w, 24, 1) == u->cond[1];
    switch (BITS(w, 22, 2)) {
    case 0: return x || y;
    case 1: return x && y;
    case 2: return x;
    default: return y;
    }
}

typedef struct { uint32_t else_at, end_at; } IfEntry;
typedef struct { uint32_t end_at, return_to; } CallEntry;
typedef struct { uint32_t entry, end_at; uint8_t down, step, previous_al; } LoopEntry;

/* Azahar's stacks are boost::circular_buffer: a push past capacity drops the oldest entry */
#define PUSH(stack, n, cap, value) do { if ((n) == (cap)) { memmove(stack, stack + 1, sizeof(stack[0]) * ((cap) - 1)); (n)--; } stack[(n)++] = (value); } while (0)

int pica_shader_run(const PicaShaderSetup* s, PicaShaderUnit* u, uint32_t entry, uint32_t max_steps)
{
    IfEntry ifs[8];
    CallEntry calls[4];
    LoopEntry loops[4];
    int nif = 0, ncall = 0, nloop = 0;
    uint32_t pc = entry;
    for (;;) {
        if (u->steps++ >= max_steps)
            return -1;
        int is_break = 0;
        const uint32_t old_pc = pc;
        const uint32_t w = pc < PICA_PROGRAM_WORDS - 1 ? s->code[pc] : (uint32_t)OP_END << 26;
        uint32_t op = w >> 26;
        if ((op & ~7u) == OP_MAD)
            op = OP_MAD;
        else if ((op & ~7u) == OP_MADI)
            op = OP_MADI;
        else if ((op & ~1u) == OP_CMP)
            op = OP_CMP;
        float scratch[4];

        if (op < 0x20 || op == OP_CMP) { /* arithmetic (and cmp) */
            const uint32_t desc = s->swizzle[BITS(w, 0, 7)];
            const int inverted = op == OP_DPHI || op == OP_DSTI || op == OP_SGEI || op == OP_SLTI;
            const uint32_t r1 = inverted ? BITS(w, 14, 5) : BITS(w, 12, 7);
            const uint32_t r2 = inverted ? BITS(w, 7, 7) : BITS(w, 7, 5);
            const uint32_t ar = BITS(w, 19, 2);
            float a[4], b[4];
            pick(source(s, u, r1, inverted ? 0 : ar), BITS(desc, 5, 8), BITS(desc, 4, 1), a);
            pick(source(s, u, r2, inverted ? ar : 0), BITS(desc, 14, 8), BITS(desc, 13, 1), b);
            float* d = destination(u, BITS(w, 21, 5), scratch);
            const uint32_t mask = BITS(desc, 0, 4);
#define FOR_DEST for (int k = 0; k < 4; k++) if (mask & (8u >> k))
            switch (op) {
            case OP_ADD: FOR_DEST d[k] = a[k] + b[k]; break;
            case OP_MUL: FOR_DEST d[k] = mul24(a[k], b[k]); break;
            case OP_FLR: FOR_DEST d[k] = floorf(a[k]); break;
            case OP_MAX: FOR_DEST d[k] = a[k] > b[k] ? a[k] : b[k]; break;
            case OP_MIN: FOR_DEST d[k] = a[k] < b[k] ? a[k] : b[k]; break;
            case OP_DP3: case OP_DP4: case OP_DPH: case OP_DPHI: {
                if (op == OP_DPH || op == OP_DPHI)
                    a[3] = 1;
                float dot = 0;
                for (int k = 0; k < (op == OP_DP3 ? 3 : 4); k++)
                    dot = dot + mul24(a[k], b[k]);
                FOR_DEST d[k] = dot;
                break;
            }
            case OP_RCP: { float r = 1.0f / a[0]; FOR_DEST d[k] = r; break; }
            case OP_RSQ: { float r = 1.0f / sqrtf(a[0]); FOR_DEST d[k] = r; break; }
            case OP_EX2: { float r = exp2f(a[0]); FOR_DEST d[k] = r; break; }
            case OP_LG2: { float r = log2f(a[0]); FOR_DEST d[k] = r; break; }
            case OP_MOVA: for (int k = 0; k < 2; k++) if (mask & (8u >> k)) u->address[k] = to_s32(a[k]); break;
            case OP_MOV: FOR_DEST d[k] = a[k]; break;
            case OP_SGE: case OP_SGEI: FOR_DEST d[k] = a[k] >= b[k] ? 1.0f : 0.0f; break;
            case OP_SLT: case OP_SLTI: FOR_DEST d[k] = a[k] < b[k] ? 1.0f : 0.0f; break;
            case OP_CMP:
                for (int k = 0; k < 2; k++) {
                    switch (BITS(w, k == 0 ? 24 : 21, 3)) {
                    case 0: u->cond[k] = a[k] == b[k]; break;
                    case 1: u->cond[k] = a[k] != b[k]; break;
                    case 2: u->cond[k] = a[k] < b[k]; break;
                    case 3: u->cond[k] = a[k] <= b[k]; break;
                    case 4: u->cond[k] = a[k] > b[k]; break;
                    case 5: u->cond[k] = a[k] >= b[k]; break;
                    default: break; /* (Azahar logs and leaves the code as it was) */
                    }
                }
                break;
            default: break; /* dst, lit and the unused opcodes: Azahar writes nothing */
            }
#undef FOR_DEST
        } else if (op == OP_MAD || op == OP_MADI) {
            const uint32_t desc = s->swizzle[BITS(w, 0, 5)];
            const int inverted = op == OP_MADI;
            const uint32_t r1 = BITS(w, 17, 5);
            const uint32_t r2 = inverted ? BITS(w, 12, 5) : BITS(w, 10, 7);
            const uint32_t r3 = inverted ? BITS(w, 5, 7) : BITS(w, 5, 5);
            const uint32_t ar = BITS(w, 22, 2);
            float a[4], b[4], c[4];
            pick(source(s, u, r1, 0), BITS(desc, 5, 8), BITS(desc, 4, 1), a);
            pick(source(s, u, r2, inverted ? 0 : ar), BITS(desc, 14, 8), BITS(desc, 13, 1), b);
            pick(source(s, u, r3, inverted ? ar : 0), BITS(desc, 23, 8), BITS(desc, 22, 1), c);
            float* d = destination(u, BITS(w, 24, 5), scratch);
            for (int k = 0; k < 4; k++)
                if (BITS(desc, 0, 4) & (8u >> k))
                    d[k] = mul24(a[k], b[k]) + c[k];
        } else {
            const uint32_t count = BITS(w, 0, 8), target = BITS(w, 10, 12);
            const uint32_t bool_id = BITS(w, 22, 4), int_id = BITS(w, 22, 2);
            switch (op) {
            case OP_END:
                return 0;
            case OP_JMPC:
                if (condition(u, w))
                    pc = target - 1;
                break;
            case OP_JMPU:
                if (s->b[bool_id] == !(count & 1))
                    pc = target - 1;
                break;
            case OP_CALL: case OP_CALLU: case OP_CALLC:
                if (op == OP_CALL || (op == OP_CALLU && s->b[bool_id]) || (op == OP_CALLC && condition(u, w))) {
                    PUSH(calls, ncall, 4, ((CallEntry){target + count, pc + 1}));
                    pc = target - 1;
                }
                break;
            case OP_IFU: case OP_IFC:
                if (op == OP_IFU ? s->b[bool_id] : condition(u, w))
                    PUSH(ifs, nif, 8, ((IfEntry){target, target + count}));
                else
                    pc = target - 1;
                break;
            case OP_LOOP: {
                const uint8_t* p = s->i[int_id];
                u->address[2] = p[1];
                PUSH(loops, nloop, 4, ((LoopEntry){pc + 1, target + 1, p[0], p[2], (uint8_t)u->address[2]}));
                u->address[2] = p[1];
                break;
            }
            case OP_BREAK:
                is_break = 1;
                break;
            case OP_BREAKC:
                if (condition(u, w))
                    is_break = 1;
                break;
            case OP_SETEMIT:
                u->emit_vertex = (uint8_t)BITS(w, 24, 2);
                u->emit_primitive = (uint8_t)BITS(w, 23, 1);
                u->emit_winding = (uint8_t)BITS(w, 22, 1);
                break;
            case OP_EMIT:
                if (u->emit) {
                    u->emit(u->emit_context, u);
                    float keep[16][4];
                    memcpy(keep, u->output, sizeof keep);
                    memcpy(u->output, u->other_bank, sizeof keep);
                    memcpy(u->other_bank, keep, sizeof keep);
                }
                break;
            default: /* nop */
                break;
            }
        }

        pc++;
        /* the stacks, in Azahar's order: up to 4 calls closed (the fourth leaving pc: the hardware's bug), then one if,
           then one loop, the last two checked against the instruction's own address */
        uint32_t next = old_pc + 1;
        for (int k = 0; k < 4; k++) {
            if (!ncall || calls[ncall - 1].end_at != next)
                break;
            if (k < 3) {
                pc = calls[ncall - 1].return_to;
                next = pc;
            }
            ncall--;
        }
        if (nif && ifs[nif - 1].else_at == old_pc + 1) {
            pc = ifs[nif - 1].end_at;
            nif--;
        }
        if (nloop && (loops[nloop - 1].end_at == old_pc + 1 || is_break)) {
            LoopEntry* l = &loops[nloop - 1];
            u->address[2] += l->step;
            if (!is_break && l->down--) {
                pc = l->entry;
            } else {
                pc = l->end_at;
                if (nloop > 1)
                    u->address[2] = l->previous_al;
                nloop--;
            }
        }
    }
}
