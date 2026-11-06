/* The PICA200's vertex shader as a Vulkan vertex shader: see PicaVertexShader.h. */
#include "PicaVertexShader.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "PicaShaderGlsl.h"

#define BITS(w, at, n) (((w) >> (at)) & ((1u << (n)) - 1))

typedef struct { char* p; size_t left; int full; } Out;

static void put(Out* o, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->p, o->left, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->left) {
        o->full = 1;
        o->left = 0;
        return;
    }
    o->p += n;
    o->left -= (size_t)n;
}

/* a semantic id (0x050-0x056's fields) as the GLSL the output component goes to, or NULL (Azahar's semantics) */
static const char* target(uint32_t semantic)
{
    static const char* const names[24] = {
        "pos.x", "pos.y", "pos.z", "pos.w", "normquat.x", "normquat.y", "normquat.z", "normquat.w",
        "primary_color.x", "primary_color.y", "primary_color.z", "primary_color.w", "texcoord0.x", "texcoord0.y",
        "texcoord1.x", "texcoord1.y", "texcoord0_w", NULL, "view.x", "view.y", "view.z", NULL, "texcoord2.x",
        "texcoord2.y"};
    return semantic < 24 ? names[semantic] : NULL;
}

long pica_vertex_shader_glsl(const PicaState* s, char* out, size_t size)
{
    const uint32_t* r = s->reg;
    const uint32_t inputs = BITS(r[0x2B9], 0, 4) + 1, entry = r[0x2BA] & 0xFFFF;
    const uint64_t input_map = (uint64_t)r[0x2BC] << 32 | r[0x2BB];
    const uint32_t output_mask = r[0x2BD] & 0xFFFF, outputs = BITS(r[0x04F], 0, 3);
    Out o = {out, size, 0};
    put(&o, "#version 450\n%s", PICA_SHADER_GLSL_FLOAT_CONTROLS);
    for (uint32_t a = 0; a < inputs; a++)
        put(&o, "layout(location = %u) in vec4 in_%u;\n", a, a);
    put(&o, "layout(std140, set = 0, binding = 5) uniform PicaVertexUniforms { vec4 c[96]; ivec4 i[4]; ivec4 b[4]; "
            "vec4 flip; } vs;\n"
            "layout(location = 0) out vec4 primary_color;\nlayout(location = 1) out vec2 texcoord0;\n"
            "layout(location = 2) out vec2 texcoord1;\nlayout(location = 3) out vec2 texcoord2;\n"
            "layout(location = 4) out float texcoord0_w;\nlayout(location = 5) out vec4 normquat;\n"
            "layout(location = 6) out vec3 view;\n"
            "vec4 pica_v[16];\nvec4 pica_o[16];\nint pica_b[16];\n"
            "#define pica_c vs.c\n#define pica_i vs.i\n");
    if (o.full)
        return -1;
    const long body = pica_shader_glsl(&s->vs, entry, 1u << 20, o.p, o.left);
    if (body < 0)
        return -1;
    o.p += body;
    o.left -= (size_t)body;
    put(&o, "void main() {\n    for (int k = 0; k < 16; k++) { pica_v[k] = vec4(0.0); pica_o[k] = vec4(0.0); "
            "pica_b[k] = vs.b[k >> 2][k & 3]; }\n");
    for (uint32_t a = 0; a < inputs; a++)
        put(&o, "    pica_v[%u] = in_%u;\n", (unsigned)(input_map >> (4 * a) & 0xF), a);
    put(&o, "    pica_main();\n    vec4 pos = vec4(0.0);\n    primary_color = vec4(0.0); texcoord0 = vec2(0.0); "
            "texcoord1 = vec2(0.0); texcoord2 = vec2(0.0); texcoord0_w = 0.0; normquat = vec4(0.0); view = vec3(0.0);\n");
    /* the outputs in order of the mask (ShaderUnit::WriteOutput), each component placed by its semantic (to_vertex) */
    uint32_t order[16], n = 0;
    for (uint32_t reg = 0; reg < 16; reg++)
        if (output_mask >> reg & 1)
            order[n++] = reg;
    for (uint32_t k = 0; k < outputs && k < 7 && k < n; k++)
        for (int c = 0; c < 4; c++) {
            const char* t = target(BITS(r[0x050 + k], 8 * c, 5));
            if (t)
                put(&o, "    %s = pica_o[%u].%c;\n", t, order[k], "xyzw"[c]);
        }
    put(&o, "    gl_Position = vec4(pos.x, pos.y * vs.flip.x, -pos.z, pos.w);\n}\n");
    return o.full ? -1 : (long)(size - o.left);
}

void pica_vertex_shader_uniforms(const PicaState* s, PicaVertexUniforms* u)
{
    memcpy(u->c, s->vs.f, sizeof u->c);
    for (int k = 0; k < 4; k++)
        for (int c = 0; c < 4; c++)
            u->i[k][c] = s->vs.i[k][c];
    for (int k = 0; k < 16; k++)
        u->b[k >> 2][k & 3] = s->vs.b[k];
    u->flip[0] = BITS(s->reg[0x11E], 24, 1) ? 1.0f : -1.0f;
    u->flip[1] = u->flip[2] = u->flip[3] = 0;
}

uint64_t pica_vertex_shader_key(const PicaState* s)
{
    const uint32_t* r = s->reg;
    const uint32_t words[] = {s->vs_version, r[0x2B9], r[0x2BA], r[0x2BB], r[0x2BC], r[0x2BD], r[0x04F], r[0x050],
                              r[0x051], r[0x052], r[0x053], r[0x054], r[0x055], r[0x056]};
    uint64_t h = 1469598103934665603ull;
    for (unsigned k = 0; k < sizeof words / 4; k++)
        h = (h ^ words[k]) * 1099511628211ull;
    return h;
}
