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

/* an attribute buffer (outputs in the order of the output mask) as a rasterizer vertex, by the semantics (0x050-0x056:
   Azahar's OutputVertex constructor) */
static void to_vertex(const uint32_t* r, const float attr[16][4], PicaVertex* v)
{
    memset(v, 0, sizeof *v);
    const uint32_t outputs = BITS(r[0x04F], 0, 3);
    for (uint32_t k = 0; k < outputs && k < 7; k++)
        for (int c = 0; c < 4; c++) {
            float* f = field(v, BITS(r[0x050 + k], 8 * c, 5));
            if (f)
                *f = attr[k][c];
        }
}

/* the primitive assembler (Azahar's PrimitiveAssembler): lists, strips, fans; the geometry shader's topology a list whose
   next triangle has its first two vertices swapped when the shader asked for the other winding */
typedef struct {
    uint32_t topology;
    PicaVertex buffer[2];
    int buffer_index, strip_ready, winding;
    PicaVertex* out;
    long written, max;
} Assembler;

static void add_triangle(Assembler* a, const PicaVertex* v0, const PicaVertex* v1, const PicaVertex* v2)
{
    if (a->written + 3 > a->max)
        return;
    PicaVertex tri[3] = {*v0, *v1, *v2};
    for (int k = 1; k < 3; k++) { /* AddTriangle: a quaternion opposite to the first's is negated */
        const float d = tri[0].normquat[0] * tri[k].normquat[0] + tri[0].normquat[1] * tri[k].normquat[1] +
                        tri[0].normquat[2] * tri[k].normquat[2] + tri[0].normquat[3] * tri[k].normquat[3];
        if (d < 0)
            for (int c = 0; c < 4; c++)
                tri[k].normquat[c] = -tri[k].normquat[c];
    }
    memcpy(a->out + a->written, tri, sizeof tri);
    a->written += 3;
}

static void assemble(Assembler* a, const PicaVertex* v)
{
    if (a->topology == 0 || a->topology == 3) {
        if (a->buffer_index < 2) {
            a->buffer[a->buffer_index++] = *v;
        } else {
            a->buffer_index = 0;
            if (a->topology == 3 && a->winding) {
                add_triangle(a, &a->buffer[1], &a->buffer[0], v);
                a->winding = 0;
            } else {
                add_triangle(a, &a->buffer[0], &a->buffer[1], v);
            }
        }
    } else {
        if (a->strip_ready)
            add_triangle(a, &a->buffer[0], &a->buffer[1], v);
        a->buffer[a->buffer_index] = *v;
        a->strip_ready |= a->buffer_index == 1;
        a->buffer_index = a->topology == 1 ? !a->buffer_index : 1;
    }
}

/* the geometry shader unit and its emitter: kept from one invocation and one draw to the next, as on the hardware
   (Azahar's gs_unit: temporaries, the output bank and setemit's state carry over) */
static PicaShaderUnit gs_unit;
static float emitted[3][16][4];
static struct { const uint32_t* r; Assembler* assembler; } emit_target;

static void gs_emit(void* context, PicaShaderUnit* u) /* GeometryEmitter::Emit */
{
    (void)context;
    const uint32_t output_mask = emit_target.r[0x28D] & 0xFFFF;
    if (u->emit_vertex >= 3)
        return;
    int n = 0;
    for (int reg = 0; reg < 16; reg++)
        if (output_mask >> reg & 1)
            memcpy(emitted[u->emit_vertex][n++], u->output[reg], sizeof emitted[0][0]);
    if (u->emit_primitive) {
        if (u->emit_winding)
            emit_target.assembler->winding = 1;
        for (int k = 0; k < 3; k++) {
            PicaVertex v;
            to_vertex(emit_target.r, emitted[k], &v);
            assemble(emit_target.assembler, &v);
        }
    }
}

/* the geometry pipeline (Azahar's GeometryPipeline: point, variable-primitive and fixed-primitive modes) */
typedef struct {
    int on, mode, need_index;
    uint32_t vs_outputs, main_left, total_left;
    float (*cur)[4], (*begin)[4], (*end)[4];
    float buffer[16][4];
} GsPipeline;

static void gs_run(PicaState* s)
{
    const uint32_t* r = s->reg;
    gs_unit.emit = gs_emit;
    gs_unit.steps = 0;
    pica_shader_run(&s->gs, &gs_unit, r[0x28A] & 0xFFFF, 1u << 20);
    s->gs.b[15] = 1; /* (set after every invocation: the program can tell the first of a batch) */
}

static void gs_configure(PicaState* s, GsPipeline* g)
{
    const uint32_t* r = s->reg;
    memset(g, 0, sizeof *g);
    g->on = (r[0x229] & 3) == 2;
    if (!g->on)
        return;
    g->mode = (int)BITS(r[0x252], 0, 8);
    g->vs_outputs = BITS(r[0x24A], 0, 4) + 1;
    if (g->mode == 0) { /* point: the vertices' outputs gathered into the shader's inputs */
        g->cur = g->begin = g->buffer;
        g->end = g->buffer + (BITS(r[0x289], 0, 4) + 1);
    } else if (g->mode == 1) { /* variable primitive: the count from the index buffer, the vertices into uniforms */
        g->need_index = 1;
    } else { /* fixed primitive: a fixed number of vertices into the uniforms from start_index */
        g->cur = g->begin = s->gs.f + BITS(r[0x252], 16, 8);
        g->end = g->begin + g->vs_outputs * (BITS(r[0x252], 8, 4) + 1);
    }
}

static void gs_index(PicaState* s, GsPipeline* g, uint32_t value)
{
    for (int c = 0; c < 4; c++)
        s->gs.f[0][c] = (float)value;
    g->cur = s->gs.f + 1;
    g->main_left = s->reg[0x254] + 1;
    g->total_left = value;
    g->need_index = 0;
}

static void gs_vertex(PicaState* s, GsPipeline* g, const float attr[16][4])
{
    const uint32_t* r = s->reg;
    if (g->mode == 1) {
        if (g->main_left) {
            for (uint32_t k = 0; k < g->vs_outputs && g->cur < s->gs.f + 96; k++)
                memcpy(*g->cur++, attr[k], sizeof attr[k]);
            g->main_left--;
        } else if (g->cur < s->gs.f + 96) {
            memcpy(*g->cur++, attr[0], sizeof attr[0]);
        }
        if (--g->total_left == 0) {
            g->need_index = 1;
            gs_run(s);
        }
        return;
    }
    for (uint32_t k = 0; k < g->vs_outputs && g->cur < g->end; k++)
        memcpy(*g->cur++, attr[k], sizeof attr[k]);
    if (g->cur < g->end)
        return;
    g->cur = g->begin;
    if (g->mode == 0) { /* ShaderUnit::LoadInput with the geometry shader's attribute map (0x28B, 0x28C) */
        const uint64_t map = (uint64_t)r[0x28C] << 32 | r[0x28B];
        for (uint32_t a = 0; a <= BITS(r[0x289], 0, 4); a++)
            memcpy(gs_unit.input[map >> (4 * a) & 0xF], g->buffer[a], sizeof g->buffer[a]);
    }
    gs_run(s);
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

    struct { int valid; uint32_t id; float attr[16][4]; } cache[64];
    memset(cache, 0, sizeof cache);
    int cache_pos = 0;
    Assembler assembler = {BITS(r[0x25E], 8, 2), {{{0}}}, 0, 0, 0, out, 0, max};
    GsPipeline gs;
    gs_configure(s, &gs);
    emit_target.r = r;
    emit_target.assembler = &assembler;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t vertex = i + r[0x22A];
        if (indexed)
            vertex = index16 ? (uint32_t)(indices[2 * i] | indices[2 * i + 1] << 8) : indices[i];
        if (indexed && gs.on && gs.mode == 1 && gs.need_index) {
            gs_index(s, &gs, vertex);
            continue;
        }
        float attr[16][4];
        int hit = 0;
        if (indexed)
            for (int k = 0; k < 64; k++)
                if (cache[k].valid && cache[k].id == vertex) { memcpy(attr, cache[k].attr, sizeof attr); hit = 1; break; }
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
            /* ShaderUnit::WriteOutput: the outputs in order of the mask */
            memset(attr, 0, sizeof attr);
            int n_out = 0;
            for (int reg = 0; reg < 16; reg++)
                if (output_mask >> reg & 1)
                    memcpy(attr[n_out++], u.output[reg], sizeof attr[0]);
            if (indexed) {
                cache[cache_pos].valid = 1;
                cache[cache_pos].id = vertex;
                memcpy(cache[cache_pos].attr, attr, sizeof attr);
                cache_pos = (cache_pos + 1) % 64;
            }
        }
        if (gs.on) {
            gs_vertex(s, &gs, (const float (*)[4])attr);
        } else {
            PicaVertex v;
            to_vertex(r, (const float (*)[4])attr, &v);
            assemble(&assembler, &v);
        }
    }
    emit_target.assembler = NULL; /* (the emitter writes into this draw's assembler only) */
    return assembler.written;
}
