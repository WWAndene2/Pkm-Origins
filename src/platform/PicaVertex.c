/* A draw's vertices: see PicaVertex.h. Registers as Azahar's regs_pipeline.h, regs_shader.h, regs_rasterizer.h. */
#include "PicaVertex.h"

#include <string.h>

#define BITS(w, at, n) (((w) >> (at)) & ((1u << (n)) - 1))

static const uint8_t* bytes(PicaState* s, uint32_t physical, uint32_t length)
{
    return s->memory ? (const uint8_t*)s->memory(physical, length, s->user) : NULL;
}

/* the semantics of the outputs: semantic id -> the vertex field it fills */
static float* field(PicaVertex* v, uint32_t semantic)
{
    switch (semantic) {
    case 0: case 1: case 2: case 3: return &v->position[semantic];
    case 4: case 5: case 6: case 7: return &v->normquat[semantic - 4];
    case 8: case 9: case 10: case 11: return &v->color[semantic - 8];
    case 12: case 13: return &v->texcoord0[semantic - 12];
    case 14: case 15: return &v->texcoord1[semantic - 14];
    case 16: return &v->texcoord0_w;
    case 18: case 19: case 20: return &v->view[semantic - 18];
    case 22: case 23: return &v->texcoord2[semantic - 22];
    default: return NULL;
    }
}

long pica_vertex_draw(PicaState* s, int indexed, PicaVertex* out, long max)
{
    const uint32_t* r = s->reg;
    const uint32_t base = BITS(r[0x200], 1, 28) * 16;
    const uint32_t f0 = r[0x201], f1 = r[0x202];
    const uint32_t total = BITS(f1, 28, 4) + 1, mask = BITS(f1, 16, 12);
    /* each attribute's source: offset from the base, stride, format, element count (VertexLoader's constructor) */
    uint32_t src[12] = {0}, stride[12] = {0}, format[12] = {0}, elements[12] = {0};
    for (int a = 0; a < 12; a++) {
        const uint32_t word = a < 8 ? f0 >> (4 * a) : f1 >> (4 * (a - 8));
        format[a] = word & 3;
    }
    for (int l = 0; l < 12; l++) {
        const uint32_t* L = r + 0x203 + 3 * l;
        const uint32_t count = BITS(L[2], 28, 4), byte_count = BITS(L[2], 16, 8);
        uint32_t offset = 0;
        for (uint32_t c = 0; c < count && c < 12; c++) {
            const uint32_t id = c < 8 ? BITS(L[1], 4 * c, 4) : BITS(L[2], 4 * (c - 8), 4);
            if (id < 12) {
                const uint32_t esize = format[id] == 3 ? 4 : format[id] == 2 ? 2 : 1;
                const uint32_t n = (id < 8 ? BITS(f0, 4 * id + 2, 2) : BITS(f1, 4 * (id - 8) + 2, 2)) + 1;
                offset = (offset + esize - 1) / esize * esize;
                src[id] = BITS(L[0], 0, 28) + offset;
                stride[id] = byte_count;
                elements[id] = n;
                offset += n * esize;
            } else {
                offset = (offset + 3) / 4 * 4 + (id - 11) * 4;
            }
        }
    }
    const uint32_t count = r[0x228];
    const uint32_t index_word = r[0x227];
    const int index16 = index_word >> 31 & 1;
    const uint8_t* indices = indexed ? bytes(s, base + BITS(index_word, 0, 28), count * (index16 ? 2 : 1)) : NULL;
    if (indexed && !indices)
        return -1;

    /* the vertex shader's inputs and outputs (regs_shader.h at 0x2B0) */
    const uint32_t max_input = BITS(r[0x2B9], 0, 4);
    const uint64_t input_map = (uint64_t)r[0x2BC] << 32 | r[0x2BB];
    const uint32_t output_mask = r[0x2BD] & 0xFFFF, entry = r[0x2BA] & 0xFFFF;
    const uint32_t outputs = BITS(r[0x04F], 0, 3);

    struct { int valid; uint32_t id; PicaVertex v; } cache[64];
    memset(cache, 0, sizeof cache);
    int cache_pos = 0;
    const uint32_t topology = BITS(r[0x25E], 8, 2);
    PicaVertex buffer[2];
    int buffer_index = 0, strip_ready = 0;
    long written = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t vertex = i + r[0x22A];
        if (indexed)
            vertex = index16 ? (uint32_t)(indices[2 * i] | indices[2 * i + 1] << 8) : indices[i];
        PicaVertex v;
        int hit = 0;
        if (indexed)
            for (int k = 0; k < 64; k++)
                if (cache[k].valid && cache[k].id == vertex) { v = cache[k].v; hit = 1; break; }
        if (!hit) {
            float input[16][4];
            memset(input, 0, sizeof input);
            for (uint32_t a = 0; a < total && a < 16; a++) {
                if (a >= 12 || (mask >> a & 1)) {
                    memcpy(input[a], s->default_attribute[a], sizeof input[a]);
                    continue;
                }
                if (!elements[a])
                    continue;
                const uint32_t esize = format[a] == 3 ? 4 : format[a] == 2 ? 2 : 1;
                const uint8_t* p = bytes(s, base + src[a] + stride[a] * vertex, elements[a] * esize);
                if (!p)
                    return -1;
                for (uint32_t c = 0; c < elements[a]; c++) {
                    switch (format[a]) {
                    case 0: input[a][c] = (float)(int8_t)p[c]; break;
                    case 1: input[a][c] = (float)p[c]; break;
                    case 2: { int16_t h; memcpy(&h, p + 2 * c, 2); input[a][c] = (float)h; break; }
                    default: memcpy(&input[a][c], p + 4 * c, 4); break;
                    }
                }
                for (uint32_t c = elements[a]; c < 4; c++)
                    input[a][c] = c == 3 ? 1.0f : 0.0f;
            }
            PicaShaderUnit u;
            memset(&u, 0, sizeof u);
            for (uint32_t a = 0; a <= max_input; a++)
                memcpy(u.input[input_map >> (4 * a) & 0xF], input[a], sizeof input[a]);
            pica_shader_run(&s->vs, &u, entry, 1u << 20);
            /* the outputs in order of the mask, then each component placed by its semantic */
            float out_regs[16][4];
            int n_out = 0;
            for (int reg = 0; reg < 16; reg++)
                if (output_mask >> reg & 1)
                    memcpy(out_regs[n_out++], u.output[reg], sizeof out_regs[0]);
            memset(&v, 0, sizeof v);
            for (uint32_t k = 0; k < outputs && k < 7; k++)
                for (int c = 0; c < 4; c++) {
                    float* f = field(&v, BITS(r[0x050 + k], 8 * c, 5));
                    if (f && (int)k < n_out)
                        *f = out_regs[k][c];
                }
            if (indexed) {
                cache[cache_pos].valid = 1;
                cache[cache_pos].id = vertex;
                cache[cache_pos].v = v;
                cache_pos = (cache_pos + 1) % 64;
            }
        }
        /* PrimitiveAssembler::SubmitVertex */
        PicaVertex tri[3];
        int emit = 0;
        if (topology == 0 || topology == 3) {
            if (buffer_index < 2) {
                buffer[buffer_index++] = v;
            } else {
                buffer_index = 0;
                tri[0] = buffer[0]; tri[1] = buffer[1]; tri[2] = v;
                emit = 1;
            }
        } else {
            if (strip_ready) {
                tri[0] = buffer[0]; tri[1] = buffer[1]; tri[2] = v;
                emit = 1;
            }
            buffer[buffer_index] = v;
            strip_ready |= buffer_index == 1;
            buffer_index = topology == 1 ? !buffer_index : 1;
        }
        if (emit) {
            if (written + 3 > max)
                return written;
            for (int k = 1; k < 3; k++) { /* AddTriangle: a quaternion opposite to the first's is negated */
                const float d = tri[0].normquat[0] * tri[k].normquat[0] + tri[0].normquat[1] * tri[k].normquat[1] +
                                tri[0].normquat[2] * tri[k].normquat[2] + tri[0].normquat[3] * tri[k].normquat[3];
                if (d < 0)
                    for (int c = 0; c < 4; c++)
                        tri[k].normquat[c] = -tri[k].normquat[c];
            }
            memcpy(out + written, tri, sizeof tri);
            written += 3;
        }
    }
    return written;
}
