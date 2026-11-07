/* The vertex shader on the GPU: see VulkanVertexShader.h. Registers as PicaVertex.c's (Azahar's regs_shader.h,
 * regs_rasterizer.h, regs_pipeline.h). */
#include "VulkanVertexShader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PicaShaderGlsl.h"
#include "PicaVertex.h"

#define BITS(w, at, n) (((w) >> (at)) & ((1u << (n)) - 1))

static VkVertexContext ctx;
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout pipe_layout;
static VkDescriptorPool pool;
static VkDescriptorSet set;
static VkBuffer set_out; /* the output buffer the set was written with */
static VkPipeline assemble_pipe;

/* the push constants of both passes */
typedef struct {
    uint32_t in_base, stride, unique_base, n;                     /* pass 1 (in vec4s, vec4s, floats, vertices) */
    uint32_t lo, index_base, indexed, first, topology, tris, out_base; /* pass 2 (out_base in floats) */
} Push;

/* the rings (a submission's draws take their data from them; freed by vk_vertex_reset) */
typedef struct { VkBuffer buf; VkDeviceMemory mem; void* map; VkDeviceSize size, used; } Ring;
enum { FLOATS_PER_VERTEX = sizeof(PicaVertex) / 4 };
static Ring inputs, uniforms, unique, indices;
static VkDeviceSize uniform_slot;

/* a float uniform block as the translation reads it: c0-c95, i0-i3, b0-b15 (std430) */
typedef struct { float c[96][4]; int32_t i[4][4]; int32_t b[16]; } Uniforms;

/* programs: compute pipelines by a key over the program, its entry, the input map and the outputs' placement */
enum { PROGRAMS = 512 };
static struct { uint64_t key, text; VkPipeline pipe; } programs[PROGRAMS];
static int program_count;

static uint64_t fnv_words(uint64_t h, const uint32_t* w, size_t n)
{
    for (size_t k = 0; k < n; k++)
        h = (h ^ w[k]) * 1099511628211ull;
    return h;
}

static int ring_make(Ring* r, VkDeviceSize size)
{
    r->size = size;
    r->used = 0;
    return ctx.make_buffer(&r->buf, &r->mem, &r->map, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
}

/* room for need bytes at the given alignment: the offset, or -1 */
static long long ring_take(Ring* r, VkDeviceSize need, VkDeviceSize align)
{
    const VkDeviceSize at = (r->used + align - 1) / align * align;
    if (at + need > r->size)
        return -1;
    r->used = at + need;
    return (long long)at;
}

static VkPipeline compute_pipeline(const char* glsl, size_t len)
{
    VkShaderModule m = ctx.compile(glsl, len, "comp");
    if (!m)
        return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                       VK_SHADER_STAGE_COMPUTE_BIT, m, "main"},
                                      pipe_layout};
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateComputePipelines(ctx.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    vkDestroyShaderModule(ctx.dev, m, NULL);
    return p;
}

#define BUFFERS \
    "layout(std430, binding = 0) readonly buffer In { vec4 vin[]; };\n" \
    "layout(std430, binding = 1) readonly buffer U { vec4 pica_c[96]; ivec4 pica_i[4]; int pica_b[16]; };\n" \
    "layout(std430, binding = 2) buffer Unique { float unique[]; };\n" \
    "layout(std430, binding = 3) readonly buffer I { uint idx[]; };\n" \
    "layout(std430, binding = 4) writeonly buffer O { float vout[]; };\n" \
    "layout(push_constant) uniform P { uint in_base, stride, unique_base, n, lo, index_base, indexed, first, " \
    "topology, tris, out_base; };\n"

/* pass 2, the same for every draw: a triangle's three vertices by the topology (PicaVertex.c's assemble: a list's
   3t..3t+2; a strip's triangle at vertex i = t + 2 is (i-2, i-1, i) for an even i, (i-1, i-2, i) for an odd one; a fan's
   (0, i-1, i)), copied out, the quaternions of the second and third negated when opposite to the first's
   (add_triangle) */
static const char assemble_glsl[] =
    "#version 450\n"
    "layout(local_size_x = 64) in;\n" BUFFERS
    "uint vertex_of(uint c) { return indexed != 0u ? idx[index_base + c] : first + c; }\n"
    "void main() {\n"
    "  uint t = gl_GlobalInvocationID.x;\n"
    "  if (t >= tris) return;\n"
    "  uint c[3];\n"
    "  if (topology == 1u) { uint i = t + 2u; if ((i & 1u) == 0u) { c[0] = i - 2u; c[1] = i - 1u; }\n"
    "                       else { c[0] = i - 1u; c[1] = i - 2u; } c[2] = i; }\n"
    "  else if (topology == 2u) { c[0] = 0u; c[1] = t + 1u; c[2] = t + 2u; }\n"
    "  else { c[0] = 3u * t; c[1] = 3u * t + 1u; c[2] = 3u * t + 2u; }\n"
    "  uint q0 = unique_base + (vertex_of(c[0]) - lo) * " "22u" " + 15u;\n"
    "  for (int k = 0; k < 3; k++) {\n"
    "    uint src = unique_base + (vertex_of(c[k]) - lo) * 22u, dst = out_base + (3u * t + uint(k)) * 22u;\n"
    "    for (uint f = 0u; f < 22u; f++) vout[dst + f] = unique[src + f];\n"
    "    if (k > 0) {\n"
    "      precise float d = unique[q0] * unique[src + 15u] + unique[q0 + 1u] * unique[src + 16u] +\n"
    "                unique[q0 + 2u] * unique[src + 17u] + unique[q0 + 3u] * unique[src + 18u];\n"
    "      if (d < 0.0) for (uint f = 15u; f < 19u; f++) vout[dst + f] = -unique[src + f];\n"
    "    }\n"
    "  }\n"
    "}\n";

static int state = -2; /* -2 not tried, -1 off, 0 on */

int vk_vertex_init(const VkVertexContext* context)
{
    if (state != -2)
        return state;
    state = -1;
    if (getenv("HOS_VERTEX_CPU"))
        return -1;
    ctx = *context;
    _Static_assert(sizeof(PicaVertex) == 22 * 4, "the passes' 22 floats a vertex");
    _Static_assert(offsetof(PicaVertex, normquat) == 15 * 4, "the quaternion at float 15");
    VkDescriptorSetLayoutBinding b[5];
    for (uint32_t k = 0; k < 5; k++)
        b[k] = (VkDescriptorSetLayoutBinding){k, k == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                                                        : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 5, b};
    if (vkCreateDescriptorSetLayout(ctx.dev, &dli, NULL, &set_layout) != VK_SUCCESS)
        return -1;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &set_layout, 1, &pr};
    if (vkCreatePipelineLayout(ctx.dev, &pli, NULL, &pipe_layout) != VK_SUCCESS)
        return -1;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, ps};
    if (vkCreateDescriptorPool(ctx.dev, &dpi, NULL, &pool) != VK_SUCCESS)
        return -1;
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &set_layout};
    if (vkAllocateDescriptorSets(ctx.dev, &dai, &set) != VK_SUCCESS)
        return -1;
    uniform_slot = (sizeof(Uniforms) + ctx.ssbo_align - 1) / ctx.ssbo_align * ctx.ssbo_align;
    if (ring_make(&inputs, 32 << 20) || ring_make(&uniforms, uniform_slot * 4096) || ring_make(&unique, 64 << 20) ||
        ring_make(&indices, 8 << 20))
        return -1;
    assemble_pipe = compute_pipeline(assemble_glsl, sizeof assemble_glsl - 1);
    if (!assemble_pipe)
        return -1;
    state = 0;
    return 0;
}

int vk_vertex_handles(const PicaState* s, int indexed)
{
    return state == 0 && indexed != 2 && (s->reg[0x229] & 3) != 2;
}

void vk_vertex_reset(void)
{
    inputs.used = uniforms.used = unique.used = indices.used = 0;
}

/* the semantics of the outputs: semantic id -> the PicaVertex float it fills (PicaVertex.c's field), or -1 */
static int field_index(uint32_t semantic)
{
    switch (semantic) {
    case 0: case 1: case 2: case 3: return (int)(offsetof(PicaVertex, position) / 4 + semantic);
    case 4: case 5: case 6: case 7: return (int)(offsetof(PicaVertex, normquat) / 4 + semantic - 4);
    case 8: case 9: case 10: case 11: return (int)(offsetof(PicaVertex, color) / 4 + semantic - 8);
    case 12: case 13: return (int)(offsetof(PicaVertex, texcoord0) / 4 + semantic - 12);
    case 14: case 15: return (int)(offsetof(PicaVertex, texcoord1) / 4 + semantic - 14);
    case 16: return (int)(offsetof(PicaVertex, texcoord0_w) / 4);
    case 18: case 19: case 20: return (int)(offsetof(PicaVertex, view) / 4 + semantic - 18);
    case 22: case 23: return (int)(offsetof(PicaVertex, texcoord2) / 4 + semantic - 22);
    default: return -1;
    }
}

/* pass 1's pipeline for the draw's program and its placement of inputs and outputs */
static VkPipeline program_for(PicaState* s)
{
    const uint32_t* r = s->reg;
    const uint32_t max_input = BITS(r[0x2B9], 0, 4);
    const uint64_t input_map = (uint64_t)r[0x2BC] << 32 | r[0x2BB];
    const uint32_t output_mask = r[0x2BD] & 0xFFFF, entry = r[0x2BA] & 0xFFFF;
    const uint32_t outputs = BITS(r[0x04F], 0, 3);
    const int flipped = !BITS(r[0x11E], 24, 1);
    const uint32_t layout[] = {max_input, (uint32_t)input_map, (uint32_t)(input_map >> 32), output_mask, entry,
                               outputs, r[0x050], r[0x051], r[0x052], r[0x053], r[0x054], r[0x055], r[0x056],
                               (uint32_t)flipped};
    uint64_t key = fnv_words(1469598103934665603ull, layout, sizeof layout / 4);
    const uint64_t code_key = pica_shader_glsl_key(&s->vs, entry);
    key = fnv_words(key, (const uint32_t*)&code_key, 2);
    /* (the boolean and integer uniforms: the unrolled form is made for their values) */
    uint32_t flow_uniforms[16 + 4];
    for (int k = 0; k < 16; k++)
        flow_uniforms[k] = s->vs.b[k];
    for (int k = 0; k < 4; k++)
        flow_uniforms[16 + k] = (uint32_t)s->vs.i[k][0] | (uint32_t)s->vs.i[k][1] << 8 | (uint32_t)s->vs.i[k][2] << 16;
    key = fnv_words(key, flow_uniforms, 20);
    for (int k = 0; k < program_count; k++)
        if (programs[k].key == key)
            return programs[k].pipe;

    static char body[1 << 20], glsl[(1 << 20) + 8192];
    if (pica_shader_glsl_baked(&s->vs, entry, 1u << 20, body, sizeof body) < 0 &&
        pica_shader_glsl(&s->vs, entry, 1u << 20, body, sizeof body) < 0)
        return VK_NULL_HANDLE;
    /* the inputs placed by the map (ShaderUnit::LoadInput), the outputs in the order of the output mask placed by
       their semantics (to_vertex: later semantics over earlier ones, as there), the rest of the vertex 0 */
    char load[2048] = "", store[8192] = "";
    size_t lp = 0, sp = 0;
    for (uint32_t a = 0; a <= max_input; a++)
        lp += (size_t)snprintf(load + lp, sizeof load - lp, "  pica_v[%u] = vin[in_base + v * stride + %uu];\n",
                               (unsigned)(input_map >> (4 * a) & 0xF), a);
    int reg_of[16], n_out = 0;
    for (int reg = 0; reg < 16; reg++)
        if (output_mask >> reg & 1)
            reg_of[n_out++] = reg;
    for (uint32_t k = 0; k < outputs && k < 7; k++)
        for (int c = 0; c < 4; c++) {
            const int f = field_index(BITS(r[0x050 + k], 8 * c, 5));
            if (f < 0)
                continue;
            /* (an output past the mask's set registers: the CPU's attribute buffer holds 0 there) */
            if ((int)k < n_out)
                sp += (size_t)snprintf(store + sp, sizeof store - sp, "  unique[o + %du] = pica_o[%d][%d];\n", f,
                                       reg_of[k], c);
            else
                sp += (size_t)snprintf(store + sp, sizeof store - sp, "  unique[o + %du] = 0.0;\n", f);
        }
    if (flipped) /* (Azahar's trivial vertex shader: vtx_pos.y = -vtx_pos.y) */
        sp += (size_t)snprintf(store + sp, sizeof store - sp, "  unique[o + 1u] = -unique[o + 1u];\n");
    const int len = snprintf(glsl, sizeof glsl,
                             "#version 450\n%s"
                             "layout(local_size_x = 64) in;\n" BUFFERS
                             "vec4 pica_v[16]; vec4 pica_o[16];\n%s\n"
                             "void main() {\n"
                             "  uint v = gl_GlobalInvocationID.x;\n"
                             "  if (v >= n) return;\n"
                             "  for (int k = 0; k < 16; k++) { pica_v[k] = vec4(0.0); pica_o[k] = vec4(0.0); }\n"
                             "%s"
                             "  pica_main();\n"
                             "  uint o = unique_base + v * 22u;\n"
                             "  for (uint f = 0u; f < 22u; f++) unique[o + f] = 0.0;\n"
                             "%s}\n",
                             PICA_SHADER_GLSL_FLOAT_CONTROLS, body, load, store);
    if (len < 0 || (size_t)len >= sizeof glsl)
        return VK_NULL_HANDLE;
    /* (by its text too: keys that differ can give one text, and a pipeline is costly to make) */
    uint64_t text = 1469598103934665603ull;
    for (int k = 0; k < len; k++)
        text = (text ^ (uint8_t)glsl[k]) * 1099511628211ull;
    VkPipeline p = VK_NULL_HANDLE;
    for (int k = 0; k < program_count && !p; k++)
        if (programs[k].text == text)
            p = programs[k].pipe;
    if (!p)
        p = compute_pipeline(glsl, (size_t)len);
    if (p && program_count < PROGRAMS) {
        programs[program_count].text = text;
        programs[program_count].key = key;
        programs[program_count++].pipe = p;
    }
    return p;
}

static void compute_barrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         NULL, 0, NULL);
}

long vk_vertex_record(PicaState* s, int indexed, VkCommandBuffer cmd, VkBuffer out, VkDeviceSize out_offset, long max)
{
    const uint32_t* r = s->reg;
    if (!vk_vertex_handles(s, indexed))
        return VK_VERTEX_CPU;
    PicaLoader loader;
    if (pica_loader_setup(s, indexed, &loader))
        return -1;
    const uint32_t topology = BITS(r[0x25E], 8, 2), count = loader.count;
    uint32_t tris = topology == 1 || topology == 2 ? (count >= 3 ? count - 2 : 0) : count / 3;
    if ((long)tris * 3 > max)
        tris = (uint32_t)(max / 3);
    if (!tris)
        return 0;
    /* the vertices the triangles use, their lowest and highest numbers (the CPU path also runs a list's vertices past
       its last whole triangle, which make nothing) */
    const uint32_t used = topology == 1 || topology == 2 ? tris + 2 : tris * 3;
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t i = 0; i < used; i++) {
        const uint32_t v = pica_loader_vertex(&loader, i);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    const uint32_t n = hi - lo + 1, stride = BITS(r[0x2B9], 0, 4) + 1;
    if ((VkDeviceSize)n * stride * 16 > inputs.size || (VkDeviceSize)n * 88 > unique.size ||
        (VkDeviceSize)used * 4 > indices.size)
        return VK_VERTEX_CPU; /* (too large for the rings even empty) */
    const long long in_at = ring_take(&inputs, (VkDeviceSize)n * stride * 16, 16);
    const long long u_at = ring_take(&uniforms, uniform_slot, uniform_slot);
    const long long q_at = ring_take(&unique, (VkDeviceSize)n * 88, 16);
    const long long i_at = indexed ? ring_take(&indices, (VkDeviceSize)used * 4, 16) : 0;
    if (in_at < 0 || u_at < 0 || q_at < 0 || i_at < 0)
        return VK_VERTEX_FULL;
    VkPipeline program = program_for(s);
    if (!program)
        return VK_VERTEX_CPU;

    /* the inputs: each vertex the draw uses, read once (a sparse index range leaves the others 0) */
    float (*in)[4] = (float (*)[4])((uint8_t*)inputs.map + in_at);
    static uint8_t seen[1 << 16];
    const int track = indexed && n <= sizeof seen;
    if (track)
        memset(seen, 0, n);
    else if (indexed)
        memset(in, 0, (size_t)n * stride * 16);
    uint32_t* idx = indexed ? (uint32_t*)((uint8_t*)indices.map + i_at) : NULL;
    for (uint32_t i = 0; i < used; i++) {
        const uint32_t v = pica_loader_vertex(&loader, i);
        if (idx)
            idx[i] = v;
        if (indexed && track && seen[v - lo]++)
            continue;
        float input[16][4];
        if (pica_loader_read(s, &loader, v, input))
            return -1;
        memcpy(in + (size_t)(v - lo) * stride, input, stride * 16);
    }
    if (indexed && track) /* (the vertices no triangle uses: 0, so that their results are defined) */
        for (uint32_t k = 0; k < n; k++)
            if (!seen[k])
                memset(in + (size_t)k * stride, 0, stride * 16);
    Uniforms* u = (Uniforms*)((uint8_t*)uniforms.map + u_at);
    memcpy(u->c, s->vs.f, sizeof u->c);
    for (int k = 0; k < 4; k++)
        for (int c = 0; c < 4; c++)
            u->i[k][c] = s->vs.i[k][c];
    for (int k = 0; k < 16; k++)
        u->b[k] = s->vs.b[k];

    if (set_out != out) {
        VkDescriptorBufferInfo bi[5] = {{inputs.buf, 0, VK_WHOLE_SIZE}, {uniforms.buf, 0, sizeof(Uniforms)},
                                        {unique.buf, 0, VK_WHOLE_SIZE}, {indices.buf, 0, VK_WHOLE_SIZE},
                                        {out, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[5];
        for (uint32_t k = 0; k < 5; k++)
            w[k] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, k, 0, 1,
                                          k == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                                                 : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                          NULL, &bi[k], NULL};
        vkUpdateDescriptorSets(ctx.dev, 5, w, 0, NULL);
        set_out = out;
    }
    const uint32_t dynamic = (uint32_t)u_at;
    const Push push = {(uint32_t)(in_at / 16), stride, (uint32_t)(q_at / 4), n, lo, (uint32_t)(i_at / 4),
                       (uint32_t)(indexed != 0), loader.first, topology, tris, (uint32_t)(out_offset / 4)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout, 0, 1, &set, 1, &dynamic);
    vkCmdPushConstants(cmd, pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, program);
    vkCmdDispatch(cmd, (n + 63) / 64, 1, 1);
    compute_barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, assemble_pipe);
    vkCmdDispatch(cmd, (tris + 63) / 64, 1, 1);
    return (long)tris * 3;
}
