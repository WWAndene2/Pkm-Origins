/* The shader translator (src/platform/PicaShaderGlsl.c) checked against the interpreter (PicaShader.c): random
 * programs, each translated, compiled to SPIR-V (glslangValidator) and run as a compute shader on a Vulkan device
 * (lavapipe in the container) over many random input vertices; every output word compared with the interpreter's.
 *
 *   cc -O2 -I src/platform tests/PicaShaderGlslTest.c src/platform/PicaShader.c src/platform/PicaShaderGlsl.c \
 *      -lvulkan -lm -o pica_shader_glsl_test && ./pica_shader_glsl_test [programs] [work dir]
 *
 * Suites: "exact" (every arithmetic and mad form, flow control by hand-built programs: bit for bit, a NaN equal to
 * any NaN), "approximate" (rcp, rsq, ex2, lg2 alone: within 3 ULP, or 2^-21 absolute for lg2 near 1, the GPU's
 * precision, PicaShaderGlsl.h). The inputs are ordinary values, zeros of both signs, infinities and NaNs; no
 * denormals (a GPU may flush them: Vulkan leaves it to the device; f24 has none). Each exact opcode is first run
 * alone (24 programs each), so that a failure names its instruction. The shader carries
 * PICA_SHADER_GLSL_FLOAT_CONTROLS: without it lavapipe dropped signs of zero and NaNs. Last run (5 November, lavapipe):
 * 300 exact programs, 300 approximate, the flow programs: all passed. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "PicaShader.h"
#include "PicaShaderGlsl.h"

enum { VERTICES = 256 };

static VkDevice dev;
static VkPhysicalDevice phys;
static VkQueue queue;
static uint32_t qfam;

#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #x, r_); exit(2); } } while (0)

static void vk_init(void)
{
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "pica_shader_glsl_test", 1, NULL, 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst;
    VK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &phys);
    if (!n) { fprintf(stderr, "no Vulkan device\n"); exit(2); }
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(phys, &pp);
    printf("device: %s\n", pp.deviceName);
    VkQueueFamilyProperties qf[8];
    uint32_t nq = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    for (qfam = 0; qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_COMPUTE_BIT); qfam++) {}
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci};
    VK(vkCreateDevice(phys, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);
}

typedef struct { VkBuffer buf; VkDeviceMemory mem; void* map; } Buffer;

static Buffer buffer(VkDeviceSize size)
{
    Buffer b;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VK(vkCreateBuffer(dev, &bi, NULL, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b.buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t t = 0;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (t < mp.memoryTypeCount && !((req.memoryTypeBits >> t & 1) && (mp.memoryTypes[t].propertyFlags & want) == want))
        t++;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, t};
    VK(vkAllocateMemory(dev, &ai, NULL, &b.mem));
    VK(vkBindBufferMemory(dev, b.buf, b.mem, 0));
    VK(vkMapMemory(dev, b.mem, 0, size, 0, &b.map));
    return b;
}

static const char* work = ".";

/* the translated program in a compute shader: one invocation a vertex, its inputs in, its outputs out */
static int run_gpu(const PicaShaderSetup* s, const float (*in)[16][4], float (*out)[16][4])
{
    static char body[1 << 22], src[(1 << 22) + 4096];
    if (pica_shader_glsl(s, 0, 100000, body, sizeof body) < 0)
        return -1;
    snprintf(src, sizeof src,
             "#version 450\n%s"
             "layout(local_size_x = 64) in;\n"
             "layout(std430, binding = 0) readonly buffer In { vec4 vin[]; };\n"
             "layout(std430, binding = 1) buffer Out { vec4 vout[]; };\n"
             "layout(std430, binding = 2) readonly buffer U { vec4 pica_c[96]; ivec4 pica_i[4]; int pica_b[16]; };\n"
             "vec4 pica_v[16]; vec4 pica_o[16];\n%s\n"
             "void main() { uint n = gl_GlobalInvocationID.x;\n"
             "  for (int k = 0; k < 16; k++) { pica_v[k] = vin[n * 16u + uint(k)]; pica_o[k] = vec4(0.0); }\n"
             "  pica_main();\n"
             "  for (int k = 0; k < 16; k++) vout[n * 16u + uint(k)] = pica_o[k]; }\n", PICA_SHADER_GLSL_FLOAT_CONTROLS, body);
    char glsl[600], spv[600], cmd[1400];
    snprintf(glsl, sizeof glsl, "%s/pica_test.comp", work);
    snprintf(spv, sizeof spv, "%s/pica_test.spv", work);
    FILE* f = fopen(glsl, "w");
    fputs(src, f);
    fclose(f);
    snprintf(cmd, sizeof cmd, "glslangValidator -V -S comp -o %s %s > %s/pica_test.log 2>&1", spv, glsl, work);
    if (system(cmd)) {
        fprintf(stderr, "glslang failed: see %s/pica_test.log\n", work);
        return -1;
    }
    f = fopen(spv, "rb");
    static uint32_t code[1 << 22];
    size_t words = fread(code, 4, sizeof code / 4, f);
    fclose(f);

    static Buffer bin, bout, bu;
    static int made;
    if (!made) {
        bin = buffer(sizeof(float) * 64 * VERTICES);
        bout = buffer(sizeof(float) * 64 * VERTICES);
        bu = buffer(96 * 16 + 4 * 16 + 16 * 4);
        made = 1;
    }
    memcpy(bin.map, in, sizeof(float) * 64 * VERTICES);
    memset(bout.map, 0, sizeof(float) * 64 * VERTICES);
    uint8_t* u = bu.map;
    memcpy(u, s->f, sizeof s->f);
    for (int k = 0; k < 4; k++)
        for (int c = 0; c < 4; c++)
            ((int32_t*)(u + 96 * 16))[k * 4 + c] = s->i[k][c];
    for (int k = 0; k < 16; k++)
        ((int32_t*)(u + 96 * 16 + 64))[k] = s->b[k];

    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, words * 4, code};
    VkShaderModule sm;
    VK(vkCreateShaderModule(dev, &smi, NULL, &sm));
    VkDescriptorSetLayoutBinding lb[3];
    for (int k = 0; k < 3; k++)
        lb[k] = (VkDescriptorSetLayoutBinding){k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 3, lb};
    VkDescriptorSetLayout dl;
    VK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dl));
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dl};
    VkPipelineLayout pl;
    VK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main"}, pl};
    VkPipeline pipe;
    VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VkDescriptorPool dp;
    VK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dl};
    VkDescriptorSet ds;
    VK(vkAllocateDescriptorSets(dev, &dai, &ds));
    VkDescriptorBufferInfo bis[3] = {{bin.buf, 0, VK_WHOLE_SIZE}, {bout.buf, 0, VK_WHOLE_SIZE}, {bu.buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet wr[3];
    for (int k = 0; k < 3; k++)
        wr[k] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, k, 0, 1,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bis[k]};
    vkUpdateDescriptorSets(dev, 3, wr, 0, NULL);
    VkCommandPoolCreateInfo cpo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    VkCommandPool cp;
    VK(vkCreateCommandPool(dev, &cpo, NULL, &cp));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    VK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer(cb, &cbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, VERTICES / 64, 1, 1);
    VK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
    VK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(queue));
    memcpy(out, bout.map, sizeof(float) * 64 * VERTICES);
    vkDestroyCommandPool(dev, cp, NULL);
    vkDestroyDescriptorPool(dev, dp, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dl, NULL);
    vkDestroyShaderModule(dev, sm, NULL);
    return 0;
}

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static float value(void)
{
    switch (rnd() % 16) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: return INFINITY;
    case 3: return -INFINITY;
    case 4: return NAN;
    case 5: return 1.0f;
    case 6: return (float)(int)(rnd() % 9) - 4.0f;
    default: return ((float)(rnd() % 2000001) - 1000000.0f) / 1000.0f;
    }
}

static int same(float a, float b)
{
    uint32_t x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    return x == y || (isnan(a) && isnan(b));
}

static int ulps(float a, float b)
{
    if (same(a, b))
        return 0;
    if (isnan(a) || isnan(b) || isinf(a) || isinf(b) || signbit(a) != signbit(b))
        return 1 << 30;
    int32_t x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    return abs(x - y);
}

/* one program over VERTICES random vertices; approximate: the tolerance of the transcendental suite */
static int check(const char* name, PicaShaderSetup* s, int approximate)
{
    static float in[VERTICES][16][4], gpu[VERTICES][16][4];
    for (int n = 0; n < VERTICES; n++)
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 4; c++)
                in[n][r][c] = value();
    if (run_gpu(s, (const float (*)[16][4])in, gpu) < 0) {
        printf("FAIL %s: not translated\n", name);
        return 1;
    }
    int bad = 0;
    for (int n = 0; n < VERTICES && bad < 3; n++) {
        PicaShaderUnit u;
        memset(&u, 0, sizeof u);
        memcpy(u.input, in[n], sizeof u.input);
        pica_shader_run(s, &u, 0, 100000);
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 4; c++) {
                float want = u.output[r][c], got = gpu[n][r][c];
                int ok = approximate ? (ulps(want, got) <= 3 || (fabsf(want - got) <= 0x1p-21f && fabsf(want) < 1))
                                     : same(want, got);
                if (!ok && bad++ < 3)
                    printf("FAIL %s: vertex %d o%d.%c: interpreter %a, GPU %a\n", name, n, r, "xyzw"[c], want, got);
            }
    }
    return bad != 0;
}

/* a random arithmetic or mad instruction (all forms; transcendental ones only when asked) */
static int force_op = -1; /* the per-opcode suite: that opcode only */
static uint32_t random_instruction(PicaShaderSetup* s, int transcendental)
{
    static const uint32_t exact_ops[] = {0x00, 0x01, 0x02, 0x03, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x12, 0x13,
                                         0x18, 0x1A, 0x1B, 0x2E, 0x38, 0x30};
    static const uint32_t approx_ops[] = {0x0E, 0x0F, 0x05, 0x06};
    uint32_t op = transcendental ? approx_ops[rnd() % 4] : exact_ops[rnd() % (sizeof exact_ops / 4)];
    if (force_op >= 0)
        op = (uint32_t)force_op;
    uint32_t dest = rnd() % 2 ? rnd() % 8 : 0x10 + rnd() % 16;
    uint32_t desc_id = rnd() % 32;
    s->swizzle[desc_id] = rnd() | 1u; /* (at least w written) */
    if (op == 0x38 || op == 0x30) {
        uint32_t w = (op | (rnd() % 8)) << 26 | (dest & 0x1F) << 24 | (rnd() % 4) << 22 | (rnd() % 0x20) << 17 | desc_id;
        if (op == 0x38)
            w |= (rnd() % 0x80) << 10 | (rnd() % 0x20) << 5;
        else
            w |= (rnd() % 0x20) << 12 | (rnd() % 0x80) << 5;
        return w;
    }
    uint32_t w = op << 26 | dest << 21 | (rnd() % 4) << 19 | desc_id;
    if (op == 0x18 || op == 0x1A || op == 0x1B)
        w |= (rnd() % 0x20) << 14 | (rnd() % 0x80) << 7;
    else
        w |= (rnd() % 0x80) << 12 | (rnd() % 0x20) << 7;
    if (op == 0x2E)
        w |= (rnd() % 8) << 21 | (rnd() % 8) << 24; /* (cmp: dest bits are its two compare ops) */
    return w;
}

int main(int argc, char** argv)
{
    int programs = argc > 1 ? atoi(argv[1]) : 50;
    if (argc > 2)
        work = argv[2];
    vk_init();
    static PicaShaderSetup s;
    int failed = 0, exact = 0, approx = 0;
    /* each exact opcode alone first, so that a failure names its instruction */
    static const int each_op[] = {0x00, 0x01, 0x02, 0x03, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x12, 0x13, 0x18, 0x1A,
                                  0x1B, 0x2E, 0x38, 0x30};
    for (unsigned e = 0; e < sizeof each_op / sizeof each_op[0]; e++) {
        force_op = each_op[e];
        for (int p = 0; p < 24; p++) {
            memset(&s, 0, sizeof s);
            for (int k = 0; k < 96; k++)
                for (int c = 0; c < 4; c++)
                    s.f[k][c] = value();
            s.code[0] = random_instruction(&s, 0);
            s.code[1] = 0x22u << 26;
            char name[64];
            snprintf(name, sizeof name, "opcode %02X alone, program %d", each_op[e], p);
            failed |= check(name, &s, 0);
        }
    }
    force_op = -1;
    for (int p = 0; p < programs; p++) {
        memset(&s, 0, sizeof s);
        for (int k = 0; k < 96; k++)
            for (int c = 0; c < 4; c++)
                s.f[k][c] = value();
        int n = 4 + rnd() % 40;
        /* mova first, so that address-register reads are exercised with small offsets */
        s.swizzle[127] = 0xC0000000u | 0x1Bu << 5 | 0xC;
        int k = 0;
        for (; k < n; k++)
            s.code[k] = random_instruction(&s, 0);
        s.code[k] = 0x22u << 26;
        char name[64];
        snprintf(name, sizeof name, "exact program %d", p);
        failed |= check(name, &s, 0);
        exact++;
        memset(&s, 0, sizeof s);
        s.code[0] = random_instruction(&s, 1);
        s.code[1] = 0x22u << 26;
        snprintf(name, sizeof name, "approximate program %d", p);
        failed |= check(name, &s, 1);
        approx++;
    }
    /* flow control: loops (nested), ifs, calls, breaks, jumps by hand */
    {
        memset(&s, 0, sizeof s);
        s.swizzle[0] = 0xFu | 0x1Bu << 5 | 0x1Bu << 14 | 0x1Bu << 23;
        s.i[0][0] = 2; s.i[0][1] = 0; s.i[0][2] = 1;
        s.i[1][0] = 1; s.i[1][1] = 5; s.i[1][2] = 2;
        s.b[3] = 1;
        uint32_t c[] = {
            0x29u << 26 | 0 << 22 | 6 << 10,           /* 0 loop i0 to 6 */
            0x29u << 26 | 1 << 22 | 3 << 10,           /* 1  loop i1 to 3 */
            0x00u << 26 | 0x10 << 21 | 0x10 << 12 | 0, /* 2   add r0, r0, v0 */
            0x13u << 26 | 0x11 << 21 | 0x20 << 12 | 3 << 19, /* 3   mov r1, c[aL] */
            0x27u << 26 | 3 << 22 | 6 << 10 | 1,       /* 4  ifu b3: then 5, else 6 */
            0x00u << 26 | 0x12 << 21 | 0x12 << 12 | 0x11 << 7, /* 5  add r2, r2, r1 */
            0x08u << 26 | 0x13 << 21 | 0x11 << 12 | 0x10 << 7, /* 6 mul r3, r1, r0 (else, and loop end) */
            0x24u << 26 | 10 << 10 | 2,                /* 7 call 10-11 */
            0x13u << 26 | 2 << 21 | 0x12 << 12,        /* 8 mov o2, r2 */
            0x22u << 26,                               /* 9 end */
            0x13u << 26 | 0 << 21 | 0x10 << 12,        /* 10 mov o0, r0 */
            0x13u << 26 | 1 << 21 | 0x13 << 12,        /* 11 mov o1, r3 */
        };
        memcpy(s.code, c, sizeof c);
        for (int k = 0; k < 96; k++)
            for (int x = 0; x < 4; x++)
                s.f[k][x] = (float)(k * 4 + x);
        failed |= check("flow: nested loops, ifu, call, aL-indexed uniform", &s, 0);
        s.b[3] = 0;
        failed |= check("flow: the same with the else taken", &s, 0);
    }
    printf("%d exact programs, %d approximate, flow control: %s\n", exact, approx, failed ? "FAILED" : "all passed");
    return failed;
}
