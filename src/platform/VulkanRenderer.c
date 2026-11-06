/* The renderer's Vulkan side: see VulkanRenderer.h. PICA200 state meanings as Azahar's regs_framebuffer.h,
 * regs_rasterizer.h, regs_texturing.h and its Vulkan rasterizer (pica_to_vk.h). */
#include "VulkanRenderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "PicaFragment.h"
#include "PicaTexture.h"
#include "PicaVertex.h"

#define BITS(w, at, n) (((w) >> (at)) & ((1u << (n)) - 1))
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "vulkan: %s: %d\n", #x, r_); return; } } while (0)
#define VKI(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "vulkan: %s: %d\n", #x, r_); return -1; } } while (0)

static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkCommandPool cmd_pool;
static VkCommandBuffer cmd;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties memprops;
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout pipe_layout;
static VkDescriptorPool desc_pool;
static VkRenderPass render_pass;
static VkShaderModule vertex_module;
static const char* dir;
static int ready = -2; /* -2 not tried, -1 off, 0 on */
static unsigned frame_count;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t t = 0; t < memprops.memoryTypeCount; t++)
        if ((bits >> t & 1) && (memprops.memoryTypes[t].propertyFlags & want) == want)
            return t;
    return 0;
}

typedef struct { VkBuffer buf; VkDeviceMemory mem; void* map; VkDeviceSize size; } Buffer;

static int make_buffer(Buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage};
    VKI(vkCreateBuffer(dev, &bi, NULL, &b->buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, b->buf, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
        memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKI(vkAllocateMemory(dev, &ai, NULL, &b->mem));
    VKI(vkBindBufferMemory(dev, b->buf, b->mem, 0));
    VKI(vkMapMemory(dev, b->mem, 0, size, 0, &b->map));
    b->size = size;
    return 0;
}

typedef struct { VkImage image; VkDeviceMemory mem; VkImageView view; } Image;

static int make_image(Image* im, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, format, {w, h, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                            VK_IMAGE_LAYOUT_UNDEFINED};
    VKI(vkCreateImage(dev, &ii, NULL, &im->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, im->image, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                               memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VKI(vkAllocateMemory(dev, &ai, NULL, &im->mem));
    VKI(vkBindImageMemory(dev, im->image, im->mem, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im->image, VK_IMAGE_VIEW_TYPE_2D,
                                format, {0}, {aspect, 0, 1, 0, 1}};
    VKI(vkCreateImageView(dev, &vi, NULL, &im->view));
    return 0;
}

static void begin(void)
{
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &bi);
}

static void submit(void)
{
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd};
    vkResetFences(dev, 1, &fence);
    vkQueueSubmit(queue, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
}

static void barrier_to_general(VkImage image, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0,
                              VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
                              {aspect, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
}

/* a shader compiled by glslangValidator, kept in dir by the text's hash */
static VkShaderModule shader(const char* glsl, size_t len, const char* stage)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t k = 0; k < len; k++)
        h = (h ^ (uint8_t)glsl[k]) * 1099511628211ull;
    char src[700], spv[700], cmdline[2200];
    snprintf(src, sizeof src, "%s/shader_%016llx.%s", dir, (unsigned long long)h, stage);
    snprintf(spv, sizeof spv, "%s.spv", src);
    FILE* f = fopen(spv, "rb");
    if (!f) {
        FILE* g = fopen(src, "w");
        if (!g) return VK_NULL_HANDLE;
        fwrite(glsl, 1, len, g);
        fclose(g);
        snprintf(cmdline, sizeof cmdline, "glslangValidator -V -o %s %s > %s.log 2>&1", spv, src, src);
        if (system(cmdline)) {
            fprintf(stderr, "renderer: %s did not compile (%s.log)\n", src, src);
            return VK_NULL_HANDLE;
        }
        f = fopen(spv, "rb");
        if (!f) return VK_NULL_HANDLE;
    }
    static uint32_t code[1 << 20];
    size_t words = fread(code, 4, sizeof code / 4, f);
    fclose(f);
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, words * 4, code};
    VkShaderModule m;
    return vkCreateShaderModule(dev, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static const char vertex_glsl[] =
    "#version 450\n"
    "layout(location = 0) in vec4 vert_position;\nlayout(location = 1) in vec4 vert_color;\n"
    "layout(location = 2) in vec2 vert_texcoord0;\nlayout(location = 3) in vec2 vert_texcoord1;\n"
    "layout(location = 4) in vec2 vert_texcoord2;\nlayout(location = 5) in float vert_texcoord0_w;\n"
    "layout(location = 6) in vec4 vert_normquat;\nlayout(location = 7) in vec3 vert_view;\n"
    "layout(location = 0) out vec4 primary_color;\nlayout(location = 1) out vec2 texcoord0;\n"
    "layout(location = 2) out vec2 texcoord1;\nlayout(location = 3) out vec2 texcoord2;\n"
    "layout(location = 4) out float texcoord0_w;\nlayout(location = 5) out vec4 normquat;\n"
    "layout(location = 6) out vec3 view;\n"
    "void main() {\n"
    "    primary_color = vert_color; texcoord0 = vert_texcoord0; texcoord1 = vert_texcoord1; texcoord2 = vert_texcoord2;\n"
    "    texcoord0_w = vert_texcoord0_w; normquat = vert_normquat; view = vert_view;\n"
    /* the PICA's clip volume has -w <= z <= 0: -z is Vulkan's 0 <= z <= w (Azahar's trivial vertex shader) */
    "    gl_Position = vec4(vert_position.x, vert_position.y, -vert_position.z, vert_position.w);\n"
    "}\n";

int vk_renderer_init(void)
{
    if (ready != -2)
        return ready;
    ready = -1;
    dir = getenv("HOS_RENDER_DIR");
    if (!dir)
        return -1;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "pkm-origins", 1, NULL, 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst;
    VKI(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &phys);
    if (!n) return -1;
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    VkQueueFamilyProperties qf[8];
    uint32_t nq = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    for (qfam = 0; qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT); qfam++) {}
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceFeatures features = {0};
    features.logicOp = VK_TRUE;
    features.fragmentStoresAndAtomics = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, &features};
    VKI(vkCreateDevice(phys, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    VKI(vkCreateCommandPool(dev, &cpi, NULL, &cmd_pool));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cmd_pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VKI(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKI(vkCreateFence(dev, &fi, NULL, &fence));

    VkDescriptorSetLayoutBinding b[5] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 5, b};
    VKI(vkCreateDescriptorSetLayout(dev, &dli, NULL, &set_layout));
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &set_layout};
    VKI(vkCreatePipelineLayout(dev, &pli, NULL, &pipe_layout));
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 4, 3, ps};
    VKI(vkCreateDescriptorPool(dev, &dpi, NULL, &desc_pool));

    /* one render pass: colour RGBA8 and depth-stencil D24S8, both loaded and kept, in the general layout */
    VkAttachmentDescription at[2] = {
        {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
         VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL},
        {0, VK_FORMAT_D24_UNORM_S8_UINT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
         VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL}};
    VkAttachmentReference cr = {0, VK_IMAGE_LAYOUT_GENERAL}, dr = {1, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &cr, NULL, &dr};
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 2, at, 1, &sp};
    VKI(vkCreateRenderPass(dev, &rpi, NULL, &render_pass));
    vertex_module = shader(vertex_glsl, sizeof vertex_glsl - 1, "vert");
    if (!vertex_module)
        return -1;
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(phys, &pp);
    fprintf(stderr, "renderer: Vulkan on %s, frames in %s\n", pp.deviceName, dir);
    ready = 0;
    return 0;
}

/* render targets, by the colour buffer's physical address */
enum { TARGETS = 16 };
static struct { uint32_t address, width, height, depth_address; Image color, depth; VkFramebuffer fb; int used; } targets[TARGETS];

static int target_for(uint32_t address, uint32_t width, uint32_t height)
{
    for (int k = 0; k < TARGETS; k++)
        if (targets[k].used && targets[k].address == address && targets[k].width == width && targets[k].height == height)
            return k;
    int k = 0;
    while (k < TARGETS && targets[k].used) k++;
    if (k == TARGETS) return -1;
    if (make_image(&targets[k].color, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT) ||
        make_image(&targets[k].depth, width, height, VK_FORMAT_D24_UNORM_S8_UINT,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        return -1;
    VkImageView views[2] = {targets[k].color.view, targets[k].depth.view};
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, render_pass, 2, views, width, height, 1};
    if (vkCreateFramebuffer(dev, &fi, NULL, &targets[k].fb) != VK_SUCCESS) return -1;
    begin();
    barrier_to_general(targets[k].color.image, VK_IMAGE_ASPECT_COLOR_BIT);
    barrier_to_general(targets[k].depth.image, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkClearColorValue black = {{0, 0, 0, 0}};
    VkImageSubresourceRange cr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, targets[k].color.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &cr);
    VkClearDepthStencilValue ds = {1, 0};
    VkImageSubresourceRange dr = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cmd, targets[k].depth.image, VK_IMAGE_LAYOUT_GENERAL, &ds, 1, &dr);
    submit();
    targets[k].address = address;
    targets[k].width = width;
    targets[k].height = height;
    targets[k].used = 1;
    return k;
}

/* textures, by address, size and format, decoded to RGBA8; their bytes hashed again once a frame and the image
 * decoded again when they changed (a game loads new textures where old ones were: the cinematic streams its scenes'
 * textures into the same buffers, which a cache by address alone kept showing from the first scene) */
enum { TEXTURES = 1024 };
static struct { uint32_t address, width, height, format, stamp; uint64_t hash; Image image; int used; } textures[TEXTURES];
static int texture_next;
static Buffer staging;
unsigned long long vk_texture_reloads, vk_texture_in_target;

static uint64_t hash_bytes(const uint8_t* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    const uint64_t* w = (const uint64_t*)p; /* (sizes are multiples of 8: 8 x 8 tiles) */
    for (size_t k = 0; k < n / 8; k++)
        h = (h ^ w[k]) * 1099511628211ull;
    return h;
}

static int texture_upload(int k, const uint8_t* data)
{
    if (!staging.buf && make_buffer(&staging, 1024 * 1024 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return 0;
    pica_texture_decode(textures[k].format, data, textures[k].width, textures[k].height, staging.map);
    begin();
    barrier_to_general(textures[k].image.image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {textures[k].width, textures[k].height, 1}};
    vkCmdCopyBufferToImage(cmd, staging.buf, textures[k].image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    submit();
    return 1;
}

static VkImageView texture_view(PicaState* s, uint32_t address, uint32_t width, uint32_t height, uint32_t format)
{
    const size_t bytes = pica_texture_size(format, width, height);
    const uint8_t* data = bytes && s->memory ? (const uint8_t*)s->memory(address, (uint32_t)bytes, s->user) : NULL;
    if (!data || !width || !height || width > 1024 || height > 1024)
        return VK_NULL_HANDLE;
    for (int k = 0; k < TARGETS; k++)
        if (targets[k].used && address >= targets[k].address &&
            address < targets[k].address + targets[k].width * targets[k].height * 4)
            vk_texture_in_target++; /* (counted: drawn-into textures are read from memory as the GSP left it) */
    for (int k = 0; k < TEXTURES; k++)
        if (textures[k].used && textures[k].address == address && textures[k].width == width &&
            textures[k].height == height && textures[k].format == format) {
            if (textures[k].stamp != frame_count) {
                textures[k].stamp = frame_count;
                const uint64_t h = hash_bytes(data, bytes);
                if (h != textures[k].hash) {
                    textures[k].hash = h;
                    vk_texture_reloads++;
                    texture_upload(k, data);
                }
            }
            return textures[k].image.view;
        }
    int k = texture_next++ % TEXTURES;
    if (textures[k].used) {
        vkDestroyImageView(dev, textures[k].image.view, NULL);
        vkDestroyImage(dev, textures[k].image.image, NULL);
        vkFreeMemory(dev, textures[k].image.mem, NULL);
        textures[k].used = 0;
    }
    if (make_image(&textures[k].image, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return VK_NULL_HANDLE;
    textures[k].address = address;
    textures[k].width = width;
    textures[k].height = height;
    textures[k].format = format;
    textures[k].stamp = frame_count;
    textures[k].hash = hash_bytes(data, bytes);
    if (!texture_upload(k, data))
        return VK_NULL_HANDLE;
    textures[k].used = 1;
    return textures[k].image.view;
}

static VkSamplerAddressMode wrap(uint32_t mode)
{
    switch (mode) {
    case 2: case 6: case 7: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 3: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; /* (to the border: the shader returns the border colour) */
    }
}

enum { SAMPLERS = 64 };
static struct { uint32_t param; VkSampler sampler; } samplers[SAMPLERS];
static int sampler_count;

static VkSampler sampler_for(uint32_t param)
{
    for (int k = 0; k < sampler_count; k++)
        if (samplers[k].param == param)
            return samplers[k].sampler;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = BITS(param, 1, 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = BITS(param, 2, 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = BITS(param, 24, 1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = wrap(BITS(param, 12, 3));
    si.addressModeV = wrap(BITS(param, 8, 3));
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(dev, &si, NULL, &s);
    if (sampler_count < SAMPLERS) {
        samplers[sampler_count].param = param;
        samplers[sampler_count++].sampler = s;
    }
    return s;
}

static VkCompareOp compare(uint32_t f)
{
    static const VkCompareOp map[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL,
                                       VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL,
                                       VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL};
    return map[f & 7];
}

static VkBlendFactor factor(uint32_t f)
{
    static const VkBlendFactor map[16] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ONE};
    return map[f & 15];
}

static VkBlendOp blend_op(uint32_t e)
{
    static const VkBlendOp map[8] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                     VK_BLEND_OP_MIN, VK_BLEND_OP_MAX, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
    return map[e & 7];
}

static VkStencilOp stencil_op(uint32_t o)
{
    static const VkStencilOp map[8] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
                                       VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                       VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                       VK_STENCIL_OP_DECREMENT_AND_WRAP};
    return map[o & 7];
}

static VkLogicOp logic_op(uint32_t o)
{
    static const VkLogicOp map[16] = {VK_LOGIC_OP_CLEAR, VK_LOGIC_OP_AND, VK_LOGIC_OP_AND_REVERSE, VK_LOGIC_OP_COPY,
                                      VK_LOGIC_OP_SET, VK_LOGIC_OP_COPY_INVERTED, VK_LOGIC_OP_NO_OP, VK_LOGIC_OP_INVERT,
                                      VK_LOGIC_OP_NAND, VK_LOGIC_OP_OR, VK_LOGIC_OP_NOR, VK_LOGIC_OP_XOR,
                                      VK_LOGIC_OP_EQUIVALENT, VK_LOGIC_OP_AND_INVERTED, VK_LOGIC_OP_OR_REVERSE,
                                      VK_LOGIC_OP_OR_INVERTED};
    return map[o & 15];
}

/* pipelines, by the fragment shader and the fixed state words */
enum { PIPELINES = 512 };
static struct { uint64_t key; VkPipeline pipe; } pipelines[PIPELINES];
static int pipeline_count;

static VkPipeline pipeline_for(const PicaState* s, const char* frag, long len)
{
    const uint32_t* r = s->reg;
    uint64_t key = 1469598103934665603ull;
    for (long k = 0; k < len; k++)
        key = (key ^ (uint8_t)frag[k]) * 1099511628211ull;
    const uint32_t words[] = {r[0x040], r[0x100], r[0x101], r[0x102], r[0x105], r[0x106], r[0x107], r[0x112], r[0x11E] >> 24 & 1,
                              r[0x113], r[0x114], r[0x115], r[0x116]};
    for (unsigned k = 0; k < sizeof words / 4; k++)
        key = (key ^ words[k]) * 1099511628211ull;
    for (int k = 0; k < pipeline_count; k++)
        if (pipelines[k].key == key)
            return pipelines[k].pipe;
    VkShaderModule fm = shader(frag, (size_t)len, "frag");
    if (!fm)
        return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex_module, "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main"}};
    VkVertexInputBindingDescription vb = {0, sizeof(PicaVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[8] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PicaVertex, position)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PicaVertex, color)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PicaVertex, texcoord0)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PicaVertex, texcoord1)},
        {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PicaVertex, texcoord2)},
        {5, 0, VK_FORMAT_R32_SFLOAT, offsetof(PicaVertex, texcoord0_w)},
        {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PicaVertex, normquat)},
        {7, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PicaVertex, view)}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &vb, 8, va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1;
    /* culling (0x040: 0 keep all, 1 keep clockwise, 2 keep counter-clockwise) */
    const uint32_t cull = BITS(r[0x040], 0, 2);
    /* the framebuffer's flip bit (0x11E bit 24 clear: flipped): the vertices' y negated (vk_renderer_draw) and the
       other face culled, as Azahar's PicaToVK::CullMode */
    const int flipped = !BITS(r[0x11E], 24, 1);
    rs.cullMode = cull == 0 || cull == 3 ? VK_CULL_MODE_NONE : flipped ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    rs.frontFace = cull == 2 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE; /* (PicaToVK::FrontFace) */
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    const uint32_t d = r[0x107];
    ds.depthTestEnable = BITS(d, 0, 1);
    ds.depthCompareOp = compare(BITS(d, 4, 3));
    ds.depthWriteEnable = BITS(d, 12, 1) && (r[0x115] & 2);
    const uint32_t st = r[0x105], so = r[0x106];
    ds.stencilTestEnable = BITS(st, 0, 1) && BITS(r[0x116], 0, 2) == 3;
    VkStencilOpState sos = {stencil_op(BITS(so, 0, 3)), stencil_op(BITS(so, 8, 3)), stencil_op(BITS(so, 4, 3)),
                            compare(BITS(st, 4, 3)), BITS(st, 24, 8), (r[0x115] & 1) ? BITS(st, 8, 8) : 0, BITS(st, 16, 8)};
    ds.front = ds.back = sos;
    VkPipelineColorBlendAttachmentState ba = {0};
    const uint32_t bl = r[0x101];
    const int blending = BITS(r[0x100], 8, 1);
    ba.blendEnable = blending;
    ba.colorBlendOp = blend_op(BITS(bl, 0, 3));
    ba.alphaBlendOp = blend_op(BITS(bl, 8, 3));
    ba.srcColorBlendFactor = factor(BITS(bl, 16, 4));
    ba.dstColorBlendFactor = factor(BITS(bl, 20, 4));
    ba.srcAlphaBlendFactor = factor(BITS(bl, 24, 4));
    ba.dstAlphaBlendFactor = factor(BITS(bl, 28, 4));
    ba.colorWriteMask = r[0x113] ? BITS(d, 8, 4) : 0; /* (r g b a: the same bit order as Vulkan's) */
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0,
                                              !blending, logic_op(BITS(r[0x102], 0, 4)), 1, &ba};
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 3, dyn};
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, stages, &vi, &ia,
                                       NULL, &vp, &rs, &ms, &ds, &cb, &dy, pipe_layout, render_pass, 0};
    /* $HOS_RENDER_LOOSE (a diagnostic): no culling, no depth or stencil test, to see what those tests remove */
    const char* loose = getenv("HOS_RENDER_LOOSE"); /* any of "cull", "depth", "stencil" */
    if (loose) {
        if (strstr(loose, "cull")) rs.cullMode = VK_CULL_MODE_NONE;
        if (strstr(loose, "depth")) ds.depthTestEnable = VK_FALSE;
        if (strstr(loose, "stencil")) ds.stencilTestEnable = VK_FALSE;
    }
    VkPipeline p = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, NULL, &p);
    if (pipeline_count < PIPELINES) {
        pipelines[pipeline_count].key = key;
        pipelines[pipeline_count++].pipe = p;
    }
    return p;
}

static Buffer vertices, uniforms, luts;
unsigned long long vk_skips[8]; /* draws not drawn, by reason (renderer_report prints them) */
static PicaVertex triangles[1 << 18];

void vk_renderer_draw(PicaState* s, int indexed)
{
    if (vk_renderer_init())
        return;
    const uint32_t* r = s->reg;
    const uint32_t width = BITS(r[0x11E], 0, 11), height = BITS(r[0x11E], 12, 10) + 1;
    const uint32_t color_address = BITS(r[0x11D], 0, 28) * 8;
    const int t = target_for(color_address, width, height);
    if (t < 0) { vk_skips[0]++; return; }
    targets[t].depth_address = BITS(r[0x11C], 0, 28) * 8;
    const long n = pica_vertex_draw(s, indexed, triangles, 1 << 18);
    if (n <= 0) { vk_skips[n < 0 ? 1 : 2]++; return; }
    static char frag[1 << 17];
    if (getenv("HOS_RENDER_LOOSE") && strstr(getenv("HOS_RENDER_LOOSE"), "alpha"))
        pica_fragment_no_alpha_test = 1;
    const long len = pica_fragment_glsl(s, frag, sizeof frag);
    if (len < 0)
        return;
    VkPipeline p = pipeline_for(s, frag, len);
    if (!p) { vk_skips[3]++; return; }
    if (!vertices.buf && (make_buffer(&vertices, sizeof triangles, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
                          make_buffer(&uniforms, sizeof(PicaFragmentUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
                          make_buffer(&luts, 24 * 256 * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)))
        return;
    if (!BITS(r[0x11E], 24, 1)) /* flipped (Azahar's trivial vertex shader: vtx_pos.y = -vtx_pos.y) */
        for (long k = 0; k < n; k++)
            triangles[k].position[1] = -triangles[k].position[1];
    memcpy(vertices.map, triangles, (size_t)n * sizeof(PicaVertex));
    pica_fragment_uniforms(s, uniforms.map);
    pica_fragment_luts(s, luts.map);
    /* the texture units' images, or the first render target's colour as a stand-in for a unit that has none */
    static const uint32_t addr_reg[3] = {0x085, 0x095, 0x09D}, dim_reg[3] = {0x082, 0x092, 0x09A},
                          param_reg[3] = {0x083, 0x093, 0x09B}, fmt_reg[3] = {0x08E, 0x096, 0x09E};
    VkDescriptorImageInfo images[3];
    for (int u = 0; u < 3; u++) {
        VkImageView v = VK_NULL_HANDLE;
        if (r[0x080] >> u & 1)
            v = texture_view(s, BITS(r[addr_reg[u]], 0, 28) * 8, BITS(r[dim_reg[u]], 16, 11), BITS(r[dim_reg[u]], 0, 11),
                             r[fmt_reg[u]] & 0xF);
        if (!v) {
            if (r[0x080] >> u & 1) vk_skips[4]++;
            v = targets[0].color.view;
        }
        images[u] = (VkDescriptorImageInfo){sampler_for(r[param_reg[u]]), v, VK_IMAGE_LAYOUT_GENERAL};
    }
    vkResetDescriptorPool(dev, desc_pool, 0);
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, desc_pool, 1, &set_layout};
    VkDescriptorSet set;
    VK(vkAllocateDescriptorSets(dev, &dai, &set));
    VkDescriptorBufferInfo ub = {uniforms.buf, 0, VK_WHOLE_SIZE}, lb = {luts.buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[5] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &ub},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &lb},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[0]},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[1]},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 4, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[2]}};
    vkUpdateDescriptorSets(dev, 5, w, 0, NULL);

    begin();
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, render_pass, targets[t].fb,
                                {{0, 0}, {width, height}}};
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout, 0, 1, &set, 0, NULL);
    /* the viewport: half sizes as float24 (0x041, 0x043), the corner (0x068) */
    const float vw = pica_float24(r[0x041] & 0xFFFFFF) * 2, vh = pica_float24(r[0x043] & 0xFFFFFF) * 2;
    VkViewport view = {(float)BITS(r[0x068], 0, 10), (float)BITS(r[0x068], 16, 10), vw, vh, 0, 1};
    vkCmdSetViewport(cmd, 0, 1, &view);
    VkRect2D scissor = {{0, 0}, {width, height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const PicaFragmentUniforms* fu = uniforms.map;
    vkCmdSetBlendConstants(cmd, fu->blend_color);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buf, &offset);
    vkCmdDraw(cmd, (uint32_t)n, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    submit();
}

void vk_renderer_display(uint32_t source)
{
    if (ready != 0)
        return;
    for (int k = 0; k < TARGETS; k++) {
        /* (a transfer may start inside the target: the game's framebuffer holds more than the screen) */
        if (!targets[k].used || source < targets[k].address ||
            source >= targets[k].address + targets[k].width * targets[k].height * 4)
            continue;
        const uint32_t w = targets[k].width, h = targets[k].height;
        Buffer out;
        if (make_buffer(&out, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
            return;
        begin();
        VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImageToBuffer(cmd, targets[k].color.image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &copy);
        submit();
        char path[700];
        snprintf(path, sizeof path, "%s/frame_%04u_%08X.ppm", dir, frame_count++, source);
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            const uint8_t* px = out.map;
            for (uint32_t y = 0; y < h; y++) /* (the PICA's framebuffer rows go up: written top row first) */
                for (uint32_t x = 0; x < w; x++)
                    fwrite(px + ((size_t)(h - 1 - y) * w + x) * 4, 1, 3, f);
            fclose(f);
        }
        vkUnmapMemory(dev, out.mem);
        vkDestroyBuffer(dev, out.buf, NULL);
        vkFreeMemory(dev, out.mem, NULL);
        return;
    }
    static int told;
    if (told++ < 4) {
        fprintf(stderr, "renderer: a display transfer from %08X, no render target there (targets:", source);
        for (int k = 0; k < TARGETS; k++)
            if (targets[k].used)
                fprintf(stderr, " %08X %ux%u", targets[k].address, targets[k].width, targets[k].height);
        fprintf(stderr, ")\n");
    }
}

void vk_renderer_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width_bits)
{
    if (ready != 0)
        return;
    for (int k = 0; k < TARGETS; k++) {
        if (!targets[k].used)
            continue;
        const uint32_t size = targets[k].width * targets[k].height * 4;
        const int color = start <= targets[k].address && end >= targets[k].address + size / 2;
        const int depth = targets[k].depth_address && start <= targets[k].depth_address &&
                          end >= targets[k].depth_address + size / 2;
        if (!color && !depth)
            continue;
        begin();
        if (color) { /* an RGBA8 pixel in memory: A B G R, the fill value R G B A from its high byte */
            VkClearColorValue c = {{(float)(value >> 24 & 0xFF) / 255.0f, (float)(value >> 16 & 0xFF) / 255.0f,
                                    (float)(value >> 8 & 0xFF) / 255.0f, (float)(value & 0xFF) / 255.0f}};
            if (width_bits != 2) { /* (a 16- or 24-bit fill of a colour target: not met yet) */
                c.float32[0] = c.float32[1] = c.float32[2] = c.float32[3] = 0;
            }
            VkImageSubresourceRange cr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, targets[k].color.image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &cr);
        } else { /* D24S8: depth the low 24 bits, stencil the high 8 */
            VkClearDepthStencilValue ds = {(float)(value & 0xFFFFFF) / 16777215.0f, value >> 24};
            VkImageSubresourceRange dr = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(cmd, targets[k].depth.image, VK_IMAGE_LAYOUT_GENERAL, &ds, 1, &dr);
        }
        submit();
    }
}
