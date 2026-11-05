/* The PICA200 fragment stages to GLSL: see PicaFragment.h. Register fields as Azahar's regs_texturing.h,
 * regs_lighting.h and regs_framebuffer.h; each generated part follows glsl_fs_shader_gen.cpp's of the same name. */
#include "PicaFragment.h"

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

static float half_to_float(uint32_t h, int mant, int exp)
{
    const int bias = 127 - ((1 << (exp - 1)) - 1);
    int32_t e = (int32_t)(h >> mant & ((1u << exp) - 1));
    uint32_t m = h & ((1u << mant) - 1), out;
    const uint32_t sign = (h >> (mant + exp) & 1) << 31;
    if (e == 0 && m == 0)
        out = sign;
    else if (e == 0) {
        e = bias + 1;
        while (!(m & (1u << mant))) { e--; m <<= 1; }
        m &= (1u << mant) - 1;
        out = sign | (uint32_t)e << 23 | m << (23 - mant);
    } else if (e == (1 << exp) - 1)
        out = sign | 0xFFu << 23 | m << (23 - mant);
    else
        out = sign | (uint32_t)(bias + e) << 23 | m << (23 - mant);
    float f;
    memcpy(&f, &out, 4);
    return f;
}

float pica_float16(uint32_t raw) { return half_to_float(raw & 0xFFFF, 10, 5); }
float pica_float20(uint32_t raw) { return half_to_float(raw & 0xFFFFF, 12, 7); }

static const uint32_t stage_reg[6] = {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8};

static void rgba8(uint32_t c, float o[4])
{
    for (int k = 0; k < 4; k++)
        o[k] = (float)(c >> (8 * k) & 0xFF) / 255.0f;
}

static void light_color(uint32_t c, float o[4])
{
    o[0] = (float)BITS(c, 20, 10) / 255.0f;
    o[1] = (float)BITS(c, 10, 10) / 255.0f;
    o[2] = (float)BITS(c, 0, 10) / 255.0f;
    o[3] = 0;
}

static int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

void pica_fragment_uniforms(const PicaState* s, PicaFragmentUniforms* u)
{
    const uint32_t* r = s->reg;
    memset(u, 0, sizeof *u);
    for (int k = 0; k < 6; k++)
        rgba8(r[stage_reg[k] + 3], u->const_color[k]);
    rgba8(r[0x0FD], u->buffer_color);
    light_color(r[0x1C0], u->global_ambient);
    rgba8(r[0x103], u->blend_color);
    u->depth[0] = pica_float24(r[0x04D] & 0xFFFFFF);
    u->depth[1] = pica_float24(r[0x04E] & 0xFFFFFF);
    u->alpha_ref[0] = (int32_t)BITS(r[0x104], 8, 8);
    static const uint32_t unit_reg[3] = {0x081, 0x091, 0x099};
    for (int k = 0; k < 3; k++) {
        rgba8(r[unit_reg[k]], u->tex_border[k]);
        u->tex_lod_bias[k] = (float)sext(BITS(r[unit_reg[k] + 3], 0, 13), 13) / 256.0f;
    }
    for (int n = 0; n < 8; n++) {
        const uint32_t* l = r + 0x140 + 0x10 * n;
        light_color(l[0], u->light[n].specular_0);
        light_color(l[1], u->light[n].specular_1);
        light_color(l[2], u->light[n].diffuse);
        light_color(l[3], u->light[n].ambient);
        u->light[n].position[0] = pica_float16(l[4] & 0xFFFF);
        u->light[n].position[1] = pica_float16(l[4] >> 16);
        u->light[n].position[2] = pica_float16(l[5] & 0xFFFF);
        u->light[n].spot_direction[0] = (float)sext(l[6] & 0x1FFF, 13) / 2047.0f;
        u->light[n].spot_direction[1] = (float)sext(l[6] >> 16 & 0x1FFF, 13) / 2047.0f;
        u->light[n].spot_direction[2] = (float)sext(l[7] & 0x1FFF, 13) / 2047.0f;
        u->light[n].atten[0] = pica_float20(l[0xA]);
        u->light[n].atten[1] = pica_float20(l[0xB]);
    }
}

void pica_fragment_luts(const PicaState* s, float out[24 * 256][2])
{
    for (int t = 0; t < 24; t++)
        for (int i = 0; i < 256; i++) {
            const uint32_t e = s->lighting_lut[t][i];
            out[t * 256 + i][0] = (float)(e & 0xFFF) / 4095.0f;
            out[t * 256 + i][1] = (float)sext(e >> 12 & 0xFFF, 12) / 4095.0f;
        }
}

/* combiner sources (Azahar's TevStageConfig::Source) */
static const char* source(uint32_t src, int stage, char* buf, size_t n)
{
    switch (src) {
    case 0: return "rounded_primary_color";
    case 1: return "primary_fragment_color";
    case 2: return "secondary_fragment_color";
    case 3: return "sample0()";
    case 4: return "sample1()";
    case 5: return "sample2()";
    case 6: return "vec4(0.0)"; /* (texture 3: procedural, not yet) */
    case 13: return "combiner_buffer";
    case 14: snprintf(buf, n, "u.const_color[%d]", stage); return buf;
    case 15: return "combiner_output";
    default: return "vec4(0.0)";
    }
}

static void color_modifier(Out* o, uint32_t mod, const char* src)
{
    static const char* sw[] = {"rgb", "rgb", "aaa", "aaa", "rrr", "rrr", "", "", "ggg", "ggg", "", "", "bbb", "bbb"};
    if (mod < 14 && sw[mod][0])
        put(o, (mod & 1) ? "(vec3(1.0) - %s.%s)" : "%s.%s", src, sw[mod]);
    else
        put(o, "vec3(0.0)");
}

static void alpha_modifier(Out* o, uint32_t mod, const char* src)
{
    static const char* c[] = {"a", "a", "r", "r", "g", "g", "b", "b"};
    put(o, (mod & 1) ? "(1.0 - %s.%s)" : "%s.%s", src, c[mod & 7]);
}

static const char* color_op(uint32_t op)
{
    switch (op) {
    case 0: return "c1";
    case 1: return "c1 * c2";
    case 2: return "c1 + c2";
    case 3: return "c1 + c2 - vec3(0.5)";
    case 4: return "mix(c2, c1, c3)";
    case 5: return "c1 - c2";
    case 6: case 7: return "vec3(dot(c1 - vec3(0.5), c2 - vec3(0.5)) * 4.0)";
    case 8: return "fma(c1, c2, c3)";
    case 9: return "min(c1 + c2, vec3(1.0)) * c3";
    default: return "vec3(0.0)";
    }
}

static const char* alpha_op(uint32_t op)
{
    switch (op) {
    case 0: return "a1";
    case 1: return "a1 * a2";
    case 2: return "a1 + a2";
    case 3: return "a1 + a2 - 0.5";
    case 4: return "mix(a2, a1, a3)";
    case 5: return "a1 - a2";
    case 8: return "fma(a1, a2, a3)";
    case 9: return "min(a1 + a2, 1.0) * a3";
    default: return "0.0";
    }
}

static void stage(Out* o, const uint32_t* r, int k)
{
    uint32_t src = r[stage_reg[k]], mod = r[stage_reg[k] + 1], ops = r[stage_reg[k] + 2];
    const uint32_t scales = r[stage_reg[k] + 4];
    if ((ops & 0xF) == 7) { /* dot3 rgba: Azahar keeps the colour half only */
        src &= 0xFFF;
        mod &= 0xFFF;
        ops &= 0xF;
    }
    const uint32_t cop = ops & 0xF, aop = ops >> 16 & 0xF;
    const int cmul = 1 << BITS(scales, 0, 2), amul = 1 << BITS(scales, 16, 2);
    const int pass = cop == 0 && aop == 0 && BITS(src, 0, 4) == 15 && BITS(src, 16, 4) == 15 &&
                     BITS(mod, 0, 4) == 0 && BITS(mod, 12, 3) == 0 && cmul == 1 && amul == 1;
    char b1[32], b2[32], b3[32];
    if (!pass) {
        put(o, "    {\n");
        for (int i = 0; i < 3; i++) {
            uint32_t s_ = BITS(src, 4 * i, 4);
            if (k == 0 && s_ == 15) /* the first stage's "previous" reads its third source (Azahar) */
                s_ = BITS(src, 8, 4);
            put(o, "    vec3 c%d = ", i + 1);
            color_modifier(o, BITS(mod, 4 * i, 4), source(s_, k, i == 0 ? b1 : i == 1 ? b2 : b3, 32));
            put(o, ";\n");
        }
        put(o, "    vec3 color_out = byteround(clamp(%s, vec3(0.0), vec3(1.0)));\n", color_op(cop));
        if (cop == 7) {
            put(o, "    float alpha_out = color_out.r;\n");
        } else {
            for (int i = 0; i < 3; i++) {
                uint32_t s_ = BITS(src, 16 + 4 * i, 4);
                if (k == 0 && s_ == 15)
                    s_ = BITS(src, 24, 4);
                put(o, "    float a%d = ", i + 1);
                alpha_modifier(o, BITS(mod, 12 + 4 * i, 3), source(s_, k, b1, 32));
                put(o, ";\n");
            }
            put(o, "    float alpha_out = byteround(clamp(%s, 0.0, 1.0));\n", alpha_op(aop));
        }
        put(o, "    combiner_output = vec4(clamp(color_out * %d.0, vec3(0.0), vec3(1.0)), clamp(alpha_out * %d.0, 0.0, 1.0));\n",
            cmul, amul);
        put(o, "    }\n");
    }
    put(o, "    combiner_buffer = next_combiner_buffer;\n");
    const uint32_t update = r[0x0E0];
    if (k < 4 && BITS(update, 8 + k, 1))
        put(o, "    next_combiner_buffer.rgb = combiner_output.rgb;\n");
    if (k < 4 && BITS(update, 12 + k, 1))
        put(o, "    next_combiner_buffer.a = combiner_output.a;\n");
}

static int sampler_supported(uint32_t config, int sampler)
{
    switch (sampler) {
    case 0: return config != 1;
    case 1: return config != 0 && config != 1 && config != 5;
    case 8: return config != 2 && config != 3;
    case 3: return config != 0 && config != 2 && config != 4;
    case 6: return config != 3;
    case 4: case 5: return config == 4 || config == 5 || config == 8;
    default: return 0;
    }
}

static float lut_scale(uint32_t v)
{
    switch (v) {
    case 0: return 1; case 1: return 2; case 2: return 4; case 3: return 8; case 6: return 0.25f; case 7: return 0.5f;
    default: return 0;
    }
}

/* a lookup table sampled at the input the registers name: d0 0, d1 1, fr 3, rb 4, rg 5, rr 6, sp 8 + light */
static void lut_value(Out* o, const uint32_t* r, int which, int table, int two_sided)
{
    static const int field[] = {0, 1, -1, 3, 4, 5, 6, -1, 2};
    static const int abs_bit[] = {1, 5, -1, 13, 17, 21, 25, -1, 9};
    const int f = field[which];
    const uint32_t input = BITS(r[0x1D1], 4 * f, 3), absolute = !BITS(r[0x1D0], abs_bit[which], 1);
    const char* index;
    switch (input) {
    case 0: index = "dot(normal, normalize(half_vector))"; break;
    case 1: index = "dot(normalize(view), normalize(half_vector))"; break;
    case 2: index = "dot(normal, normalize(view))"; break;
    case 3: index = "dot(light_vector, normal)"; break;
    case 4: index = "dot(light_vector, spot_dir)"; break;
    case 5:
        index = BITS(r[0x1C3], 4, 4) == 8
            ? "dot(normalize(half_vector) - normal * dot(normal, normalize(half_vector)), tangent)" : "0.0";
        break;
    default: index = "0.0"; break;
    }
    put(o, "(%.9g * ", lut_scale(BITS(r[0x1D2], 4 * f, 3)));
    if (absolute)
        put(o, two_sided ? "lut_unsigned(%d, abs(%s)))" : "lut_unsigned(%d, max(%s, 0.0)))", table, index);
    else
        put(o, "lut_signed(%d, %s))", table, index);
}

static void lighting(Out* o, const uint32_t* r)
{
    const uint32_t c0 = r[0x1C3], c1 = r[0x1C4], config = BITS(c0, 4, 4);
    put(o, "    vec4 diffuse_sum = vec4(0.0, 0.0, 0.0, 1.0), specular_sum = vec4(0.0, 0.0, 0.0, 1.0);\n"
           "    vec3 light_vector = vec3(0.0), refl_value = vec3(0.0), spot_dir = vec3(0.0), half_vector = vec3(0.0);\n"
           "    float light_distance = 0.0, dot_product = 0.0, clamp_highlights = 1.0, geo_factor = 1.0;\n");
    const uint32_t bump = BITS(c0, 28, 2), bump_unit = BITS(c0, 22, 2);
    if (bump == 1) {
        put(o, "    vec3 surface_normal = 2.0 * sample%u().rgb - 1.0;\n", bump_unit);
        if (!BITS(c0, 30, 1))
            put(o, "    surface_normal.z = sqrt(max(1.0 - (surface_normal.x * surface_normal.x + surface_normal.y * surface_normal.y), 0.0));\n");
        put(o, "    vec3 surface_tangent = vec3(1.0, 0.0, 0.0);\n");
    } else if (bump == 2) {
        put(o, "    vec3 surface_tangent = 2.0 * sample%u().rgb - 1.0;\n    vec3 surface_normal = vec3(0.0, 0.0, 1.0);\n", bump_unit);
    } else {
        put(o, "    vec3 surface_normal = vec3(0.0, 0.0, 1.0), surface_tangent = vec3(1.0, 0.0, 0.0);\n");
    }
    put(o, "    vec4 nq = normalize(normquat);\n"
           "    vec3 normal = quaternion_rotate(nq, surface_normal), tangent = quaternion_rotate(nq, surface_tangent);\n");
    if (BITS(c0, 0, 1))
        put(o, BITS(c0, 18, 1) ? "    vec4 shadow = vec4(1.0) - sample%u();\n" : "    vec4 shadow = sample%u();\n",
            BITS(c0, 24, 2));
    else
        put(o, "    vec4 shadow = vec4(1.0);\n");
    const uint32_t count = BITS(r[0x1C2], 0, 3) + 1;
    for (uint32_t li = 0; li < count; li++) {
        const uint32_t num = BITS(r[0x1D9], 4 * li, 3);
        const uint32_t lc = r[0x140 + 0x10 * num + 9];
        const int directional = BITS(lc, 0, 1), two_sided = BITS(lc, 1, 1), geo0 = BITS(lc, 2, 1), geo1 = BITS(lc, 3, 1);
        put(o, "    light_vector = u.light[%u].position.xyz%s;\n", num, directional ? "" : " + view");
        put(o, "    light_distance = length(light_vector);\n    light_vector = normalize(light_vector);\n"
               "    spot_dir = u.light[%u].spot_direction.xyz;\n    half_vector = normalize(view) + light_vector;\n", num);
        put(o, two_sided ? "    dot_product = abs(dot(light_vector, normal));\n"
                         : "    dot_product = max(dot(light_vector, normal), 0.0);\n");
        if (BITS(c0, 27, 1))
            put(o, "    clamp_highlights = sign(dot_product);\n");
        put(o, "    float spot_atten = ");
        if (!BITS(c1, 8 + num, 1) && sampler_supported(config, 8))
            lut_value(o, r, 8, 8 + num, two_sided);
        else
            put(o, "1.0");
        put(o, ";\n    float dist_atten = ");
        if (!BITS(c1, 24 + num, 1))
            put(o, "lut_unsigned(%u, clamp(u.light[%u].atten.y * light_distance + u.light[%u].atten.x, 0.0, 1.0))",
                16 + num, num, num);
        else
            put(o, "1.0");
        put(o, ";\n");
        if (geo0 || geo1)
            put(o, "    geo_factor = dot(half_vector, half_vector);\n"
                   "    geo_factor = geo_factor == 0.0 ? 0.0 : min(dot_product / geo_factor, 1.0);\n");
        put(o, "    vec3 specular_0 = ");
        if (!BITS(c1, 16, 1) && sampler_supported(config, 0))
            lut_value(o, r, 0, 0, two_sided);
        else
            put(o, "1.0");
        put(o, " * u.light[%u].specular_0.rgb%s;\n", num, geo0 ? " * geo_factor" : "");
        put(o, "    refl_value.r = ");
        if (!BITS(c1, 20, 1) && sampler_supported(config, 6)) lut_value(o, r, 6, 6, two_sided); else put(o, "1.0");
        put(o, ";\n    refl_value.g = ");
        if (!BITS(c1, 21, 1) && sampler_supported(config, 5)) lut_value(o, r, 5, 5, two_sided); else put(o, "refl_value.r");
        put(o, ";\n    refl_value.b = ");
        if (!BITS(c1, 22, 1) && sampler_supported(config, 4)) lut_value(o, r, 4, 4, two_sided); else put(o, "refl_value.r");
        put(o, ";\n    vec3 specular_1 = ");
        if (!BITS(c1, 17, 1) && sampler_supported(config, 1))
            lut_value(o, r, 1, 1, two_sided);
        else
            put(o, "1.0");
        put(o, " * refl_value * u.light[%u].specular_1.rgb%s;\n", num, geo1 ? " * geo_factor" : "");
        if (li == count - 1 && !BITS(c1, 19, 1) && sampler_supported(config, 3)) {
            if (BITS(c0, 2, 1)) { put(o, "    diffuse_sum.a = "); lut_value(o, r, 3, 3, two_sided); put(o, ";\n"); }
            if (BITS(c0, 3, 1)) { put(o, "    specular_sum.a = "); lut_value(o, r, 3, 3, two_sided); put(o, ";\n"); }
        }
        const int shadow_on = !BITS(c1, num, 1);
        const char* sp = BITS(c0, 16, 1) && shadow_on ? " * shadow.rgb" : "";
        const char* ss = BITS(c0, 17, 1) && shadow_on ? " * shadow.rgb" : "";
        put(o, "    diffuse_sum.rgb += ((u.light[%u].diffuse.rgb * dot_product%s) + u.light[%u].ambient.rgb) * dist_atten * spot_atten;\n",
            num, sp, num);
        put(o, "    specular_sum.rgb += (specular_0 + specular_1) * clamp_highlights * dist_atten * spot_atten%s;\n", ss);
    }
    if (BITS(c0, 19, 1)) {
        if (BITS(c0, 2, 1)) put(o, "    diffuse_sum.a *= shadow.a;\n");
        if (BITS(c0, 3, 1)) put(o, "    specular_sum.a *= shadow.a;\n");
    }
    put(o, "    diffuse_sum.rgb += u.global_ambient.rgb;\n"
           "    primary_fragment_color = clamp(diffuse_sum, vec4(0.0), vec4(1.0));\n"
           "    secondary_fragment_color = clamp(specular_sum, vec4(0.0), vec4(1.0));\n");
}

static void sampler_fn(Out* o, const uint32_t* r, int unit)
{
    static const uint32_t param_reg[3] = {0x083, 0x093, 0x09B};
    const uint32_t param = r[param_reg[unit]], type = BITS(r[0x083], 28, 3);
    const int coord = unit == 2 && BITS(r[0x080], 13, 1) ? 1 : unit;
    put(o, "vec4 sample%d() {\n", unit);
    if (!(r[0x080] >> unit & 1) || (unit == 0 && type == 5)) {
        put(o, "    return vec4(0.0);\n}\n");
        return;
    }
    /* clamp to border, as Azahar's shader emulation of it (modes 1 and 5) */
    if (BITS(param, 12, 3) == 1 || BITS(param, 12, 3) == 5)
        put(o, "    if (texcoord%d.x < 0.0 || texcoord%d.x > 1.0) return u.tex_border[%d];\n", coord, coord, unit);
    if (BITS(param, 8, 3) == 1 || BITS(param, 8, 3) == 5)
        put(o, "    if (texcoord%d.y < 0.0 || texcoord%d.y > 1.0) return u.tex_border[%d];\n", coord, coord, unit);
    if (unit == 0 && type == 3)
        put(o, "    return textureProj(tex0, vec3(texcoord0, texcoord0_w));\n}\n");
    else
        put(o, "    return textureLod(tex%d, texcoord%d, getLod(texcoord%d * vec2(textureSize(tex%d, 0))) + u.tex_lod_bias[%d]);\n}\n",
            unit, coord, coord, unit, unit);
}

static const char header[] =
    "#version 450\n"
    "#extension GL_EXT_spirv_intrinsics : require\n"
    "spirv_execution_mode(extensions = [\"SPV_KHR_float_controls\"], capabilities = [4466], 4461, 32);\n"
    "layout(location = 0) in vec4 primary_color;\n"
    "layout(location = 1) in vec2 texcoord0;\n"
    "layout(location = 2) in vec2 texcoord1;\n"
    "layout(location = 3) in vec2 texcoord2;\n"
    "layout(location = 4) in float texcoord0_w;\n"
    "layout(location = 5) in vec4 normquat;\n"
    "layout(location = 6) in vec3 view;\n"
    "layout(location = 0) out vec4 color;\n"
    "struct Light { vec4 specular_0, specular_1, diffuse, ambient, position, spot_direction, atten; };\n"
    "layout(std140, set = 0, binding = 0) uniform PicaFragmentUniforms {\n"
    "    vec4 const_color[6]; vec4 buffer_color; vec4 global_ambient; vec4 blend_color; vec4 depth; ivec4 alpha_ref;\n"
    "    vec4 tex_border[3]; vec4 tex_lod_bias; Light light[8];\n"
    "} u;\n"
    "layout(std430, set = 0, binding = 1) readonly buffer PicaLuts { vec2 lut[]; };\n"
    "layout(set = 0, binding = 2) uniform sampler2D tex0;\n"
    "layout(set = 0, binding = 3) uniform sampler2D tex1;\n"
    "layout(set = 0, binding = 4) uniform sampler2D tex2;\n"
    "vec3 quaternion_rotate(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }\n"
    "float byteround(float x) { return round(x * 255.0) * (1.0 / 255.0); }\n"
    "vec3 byteround(vec3 x) { return round(x * 255.0) * (1.0 / 255.0); }\n"
    "vec4 byteround(vec4 x) { return round(x * 255.0) * (1.0 / 255.0); }\n"
    "float getLod(vec2 coord) { vec2 d = max(abs(dFdx(coord)), abs(dFdy(coord))); return log2(max(d.x, d.y)); }\n"
    "float lut_entry(int t, int i, float delta) { vec2 e = lut[t * 256 + i]; return e.x + e.y * delta; }\n"
    "float lut_unsigned(int t, float pos) { int i = int(clamp(floor(pos * 256.0), 0.0, 255.0));"
    " return lut_entry(t, i, pos * 256.0 - float(i)); }\n"
    "float lut_signed(int t, float pos) { int i = int(clamp(floor(pos * 128.0), -128.0, 127.0));"
    " float delta = pos * 128.0 - float(i); if (i < 0) i += 256; return lut_entry(t, i, delta); }\n";

long pica_fragment_glsl(const PicaState* s, char* out, size_t size)
{
    const uint32_t* r = s->reg;
    Out o = {out, size, 0, 0};
    put(&o, "%s", header);
    for (int k = 0; k < 3; k++)
        sampler_fn(&o, r, k);
    put(&o, "void main() {\n"
            "    vec4 rounded_primary_color = byteround(primary_color);\n"
            "    vec4 primary_fragment_color = vec4(0.0), secondary_fragment_color = vec4(0.0);\n");
    const uint32_t alpha_test = r[0x104];
    const uint32_t func = (alpha_test & 1) ? BITS(alpha_test, 4, 3) : 1; /* (off: always) */
    if (func == 0) {
        put(&o, "    discard;\n}\n");
        return o.full ? -1 : (long)o.used;
    }
    /* depth: Vulkan's [0, 1] is the PICA's -z (Azahar's z_over_w), then the PICA's scale and offset */
    put(&o, "    float depth = -gl_FragCoord.z * u.depth.x + u.depth.y;\n");
    if (!BITS(r[0x06D], 0, 1)) /* 0x06D bit 0 clear: w-buffering (Azahar's DepthBuffering::WBuffering = 0) */
        put(&o, "    depth /= gl_FragCoord.w;\n");
    if ((r[0x08F] & 1) && !(r[0x1C6] & 1))
        lighting(&o, r);
    put(&o, "    vec4 combiner_buffer = vec4(0.0), next_combiner_buffer = u.buffer_color, combiner_output = vec4(0.0);\n");
    for (int k = 0; k < 6; k++)
        stage(&o, r, k);
    if (func >= 2) {
        static const char* fail[] = {"!=", "==", ">=", ">", "<=", "<"};
        put(&o, "    if (int(combiner_output.a * 255.0) %s u.alpha_ref.x) discard;\n", fail[func - 2]);
    }
    put(&o, "    gl_FragDepth = depth;\n    color = byteround(combiner_output);\n}\n");
    return o.full ? -1 : (long)o.used;
}
