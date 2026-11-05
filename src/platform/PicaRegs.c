/* The PICA200 register state: see PicaRegs.h. */
#include "PicaRegs.h"

#include <string.h>

float pica_float24(uint32_t hex)
{
    const uint32_t m = hex & 0xFFFF, sign = (hex >> 23 & 1) << 31;
    int32_t e = (int32_t)(hex >> 16 & 0x7F);
    uint32_t out;
    if (e == 0 && m == 0) {
        out = sign;
    } else if (e == 0) { /* a denormal, made the float32 of the same value */
        uint32_t mm = m;
        e = 127 - 63 + 1;
        while (!(mm & 0x10000)) {
            e--;
            mm <<= 1;
        }
        out = sign | (uint32_t)e << 23 | (mm & 0xFFFF) << 7;
    } else if (e == 0x7F) {
        out = sign | 0xFFu << 23 | m << 7;
    } else {
        out = sign | (uint32_t)(e + 127 - 63) << 23 | m << 7;
    }
    float f;
    memcpy(&f, &out, 4);
    return f;
}

void pica_state_init(PicaState* s, void (*draw)(PicaState*, int, void*),
                     const uint32_t* (*memory)(uint32_t, uint32_t, void*), void* user)
{
    memset(s, 0, sizeof *s);
    s->draw = draw;
    s->memory = memory;
    s->user = user;
}

/* the four words of a vec4 as the packing gives them: float24 (three words) or float32 (four) */
static void unpack(const uint32_t* b, int float32, float out[4])
{
    if (float32) {
        for (int k = 0; k < 4; k++)
            memcpy(&out[3 - k], &b[k], 4);
        return;
    }
    out[0] = pica_float24(b[2] & 0xFFFFFF);
    out[1] = pica_float24((b[1] & 0xFFFF) << 8 | (b[2] >> 24 & 0xFF));
    out[2] = pica_float24((b[0] & 0xFF) << 16 | (b[1] >> 16 & 0xFFFF));
    out[3] = pica_float24(b[0] >> 8);
}

/* the vertex shader's writes also reach the geometry unit unless it is configured apart or in use */
static int shared_with_gs(const PicaState* s)
{
    return !(s->reg[0x244] & 1) && (s->reg[0x229] & 3) == 0;
}

static void uniform_word(PicaState* s, int gs, uint32_t value)
{
    const uint32_t setup_reg = gs ? 0x290 : 0x2C0;
    uint32_t* q = gs ? s->gs_queue : s->vs_queue;
    int* n = gs ? &s->gs_queued : &s->vs_queued;
    const int float32 = s->reg[setup_reg] >> 31 & 1;
    q[(*n)++] = value;
    if (*n < (float32 ? 4 : 3))
        return;
    *n = 0;
    const uint32_t index = s->reg[setup_reg] & 0x7F;
    if (index >= 96) /* (out of range: ignored, as on the hardware per Azahar) */
        return;
    PicaShaderSetup* setup = gs ? &s->gs : &s->vs;
    unpack(q, float32, setup->f[index]);
    if (!gs && shared_with_gs(s))
        memcpy(s->gs.f[index], s->vs.f[index], sizeof s->vs.f[index]);
    s->reg[setup_reg] = (s->reg[setup_reg] & ~0x7Fu) | ((index + 1) & 0x7F);
}

static void write(PicaState* s, uint32_t id, uint32_t value, uint32_t mask, const uint32_t** list, uint32_t* left)
{
    if (id >= PICA_REGISTERS)
        return;
    static const uint32_t bytes[16] = {
        0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff, 0x00ff0000, 0x00ff00ff, 0x00ffff00, 0x00ffffff,
        0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff};
    const uint32_t m = bytes[mask & 15];
    s->reg[id] = (s->reg[id] & ~m) | (value & m);
    const uint32_t v = s->reg[id];
    switch (id) {
    case 0x232: s->attr_queued = 0; return;
    case 0x233: case 0x234: case 0x235: {
        s->attr_queue[s->attr_queued++] = v;
        if (s->attr_queued == 3) {
            s->attr_queued = 0;
            const uint32_t index = s->reg[0x232] & 0xF;
            unpack(s->attr_queue, 0, s->default_attribute[index]);
            s->reg[0x232] = (s->reg[0x232] & ~0xFu) | ((index + 1) & 0xF);
        }
        return;
    }
    case 0x23C: case 0x23D: { /* a jump: the list goes on in that buffer */
        const uint32_t k = id - 0x23C;
        const uint32_t size = (s->reg[0x238 + k] & 0xFFFFF) << 3, address = (s->reg[0x23A + k] & 0x1FFFFFFF) << 3;
        const uint32_t* to = s->memory ? s->memory(address, size, s->user) : 0;
        if (to) {
            *list = to;
            *left = size / 4;
        }
        return;
    }
    case 0x22E: case 0x22F:
        if (s->draw)
            s->draw(s, id == 0x22F, s->user);
        return;
    case 0x280: for (int k = 0; k < 16; k++) s->gs.b[k] = v >> k & 1; return;
    case 0x2B0:
        for (int k = 0; k < 16; k++) {
            s->vs.b[k] = v >> k & 1;
            if (shared_with_gs(s)) s->gs.b[k] = s->vs.b[k];
        }
        return;
    case 0x281: case 0x282: case 0x283: case 0x284:
        for (int c = 0; c < 4; c++) s->gs.i[id - 0x281][c] = (uint8_t)(v >> 8 * c);
        return;
    case 0x2B1: case 0x2B2: case 0x2B3: case 0x2B4:
        for (int c = 0; c < 4; c++) {
            s->vs.i[id - 0x2B1][c] = (uint8_t)(v >> 8 * c);
            if (shared_with_gs(s)) s->gs.i[id - 0x2B1][c] = s->vs.i[id - 0x2B1][c];
        }
        return;
    case 0x2BD:
        if (shared_with_gs(s)) s->reg[0x28D] = (s->reg[0x28D] & ~0xFFFFu) | (v & 0xFFFF);
        return;
    case 0x291: case 0x292: case 0x293: case 0x294: case 0x295: case 0x296: case 0x297: case 0x298:
        uniform_word(s, 1, v);
        return;
    case 0x2C1: case 0x2C2: case 0x2C3: case 0x2C4: case 0x2C5: case 0x2C6: case 0x2C7: case 0x2C8:
        uniform_word(s, 0, v);
        return;
    case 0x29C: case 0x29D: case 0x29E: case 0x29F: case 0x2A0: case 0x2A1: case 0x2A2: case 0x2A3:
        if (s->reg[0x29B] < PICA_PROGRAM_WORDS) s->gs.code[s->reg[0x29B]++] = v;
        return;
    case 0x2CC: case 0x2CD: case 0x2CE: case 0x2CF: case 0x2D0: case 0x2D1: case 0x2D2: case 0x2D3:
        if (s->reg[0x2CB] < 512) {
            s->vs.code[s->reg[0x2CB]] = v;
            if (shared_with_gs(s)) s->gs.code[s->reg[0x2CB]] = v;
            s->reg[0x2CB]++;
        }
        return;
    case 0x2A6: case 0x2A7: case 0x2A8: case 0x2A9: case 0x2AA: case 0x2AB: case 0x2AC: case 0x2AD:
        if (s->reg[0x2A5] < PICA_SWIZZLES) s->gs.swizzle[s->reg[0x2A5]++] = v;
        return;
    case 0x2D6: case 0x2D7: case 0x2D8: case 0x2D9: case 0x2DA: case 0x2DB: case 0x2DC: case 0x2DD:
        if (s->reg[0x2D5] < PICA_SWIZZLES) {
            s->vs.swizzle[s->reg[0x2D5]] = v;
            if (shared_with_gs(s)) s->gs.swizzle[s->reg[0x2D5]] = v;
            s->reg[0x2D5]++;
        }
        return;
    case 0x1C8: case 0x1C9: case 0x1CA: case 0x1CB: case 0x1CC: case 0x1CD: case 0x1CE: case 0x1CF: {
        const uint32_t type = s->reg[0x1C5] >> 8 & 0x1F, index = s->reg[0x1C5] & 0xFF;
        if (type < 24)
            s->lighting_lut[type][index] = v;
        s->reg[0x1C5] = (s->reg[0x1C5] & ~0xFFu) | ((index + 1) & 0xFF);
        return;
    }
    case 0x0E8: case 0x0E9: case 0x0EA: case 0x0EB: case 0x0EC: case 0x0ED: case 0x0EE: case 0x0EF: {
        const uint32_t index = s->reg[0x0E6] & 0x7F;
        s->fog_lut[index] = v;
        s->reg[0x0E6] = (s->reg[0x0E6] & ~0x7Fu) | ((index + 1) & 0x7F);
        return;
    }
    default:
        return;
    }
}

void pica_state_run(PicaState* s, const uint32_t* words, uint32_t count)
{
    const uint32_t* list = words;
    uint32_t left = count, at = 0;
    while (at < left) {
        if (at % 2)
            at++;
        if (at + 1 >= left)
            break;
        const uint32_t value = list[at], header = list[at + 1];
        at += 2;
        const uint32_t id = header & 0xFFFF, mask = header >> 16 & 15, extra = header >> 20 & 0xFF;
        const int group = header >> 31 & 1;
        const uint32_t* before = list;
        write(s, id, value, mask, &list, &left);
        if (list != before) { /* jumped: the new buffer from its start */
            at = 0;
            continue;
        }
        for (uint32_t k = 0; k < extra && at < left; k++) {
            const uint32_t* was = list;
            write(s, group ? id + 1 + k : id, list[at++], mask, &list, &left);
            if (list != was) {
                at = 0;
                break;
            }
        }
    }
}
