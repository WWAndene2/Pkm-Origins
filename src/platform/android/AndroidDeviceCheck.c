/* The Android app's first form (docs/RECOMPILATION_PLAN.md §7, owner's choice of 7 November: the game compiled on the
 * phone; this comes first so that the phone's two unknowns are checked before the rest is built on them):
 *
 *   - a Vulkan window on the phone's screen, and touch reaching the app;
 *   - whether the GPU keeps signed zeros, infinities and NaNs in 32-bit float shaders as asked
 *     (VkPhysicalDeviceFloatControlsProperties::shaderSignedZeroInfNanPreserveFloat32): the shader translation is
 *     exact only with it (PicaShaderGlsl.h, PICA_SHADER_GLSL_FLOAT_CONTROLS).
 *
 * The screen is green when the GPU has it, red when it has not; held a little lighter, with blue, while touched. The
 * GPU's name, its Vulkan version and the answer also go to the log (tag "PkmOrigins"). Our code only: no game data.
 */
#include <android/log.h>
#include <android_native_app_glue.h>
#include <stdlib.h>
#include <string.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "PkmOrigins", __VA_ARGS__)

static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkRenderPass pass;
static VkImageView views[8];
static VkFramebuffer framebuffers[8];
static uint32_t image_count;
static VkExtent2D extent;
static VkCommandPool pool;
static VkCommandBuffer cmd;
static VkSemaphore acquired, rendered;
static VkFence done;
static int ready, preserves_float32, touched;

/* the instance and the device, and the answer to the float question */
static int device_make(void)
{
    const char* inst_ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "pkm-origins", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, inst_ext};
    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
        return -1;
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(instance, &n, &phys);
    if (!n)
        return -1;
    VkPhysicalDeviceFloatControlsProperties fc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &fc};
    vkGetPhysicalDeviceProperties2(phys, &p2);
    preserves_float32 = fc.shaderSignedZeroInfNanPreserveFloat32 == VK_TRUE;
    LOG("GPU %s, Vulkan %u.%u.%u, driver %08X; shaderSignedZeroInfNanPreserveFloat32 %s", p2.properties.deviceName,
        VK_API_VERSION_MAJOR(p2.properties.apiVersion), VK_API_VERSION_MINOR(p2.properties.apiVersion),
        VK_API_VERSION_PATCH(p2.properties.apiVersion), p2.properties.driverVersion, preserves_float32 ? "yes" : "no");
    VkQueueFamilyProperties qf[8];
    uint32_t nq = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    for (qfam = 0; qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT); qfam++) {}
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    const char* dev_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, dev_ext};
    if (vkCreateDevice(phys, &dci, NULL, &dev) != VK_SUCCESS)
        return -1;
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    vkCreateCommandPool(dev, &cpi, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(dev, &si, NULL, &acquired);
    vkCreateSemaphore(dev, &si, NULL, &rendered);
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    vkCreateFence(dev, &fi, NULL, &done);
    return 0;
}

/* the window's surface, swapchain, render pass (cleared, presented) and framebuffers */
static int window_make(ANativeWindow* window)
{
    VkAndroidSurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, NULL, 0, window};
    if (vkCreateAndroidSurfaceKHR(instance, &sci, NULL, &surface) != VK_SUCCESS)
        return -1;
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    extent = caps.currentExtent;
    VkSurfaceFormatKHR formats[16];
    uint32_t nf = 16;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, formats);
    const VkFormat format = formats[0].format;
    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount && images > caps.maxImageCount)
        images = caps.maxImageCount;
    VkSwapchainCreateInfoKHR sw = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surface, images, format,
                                   formats[0].colorSpace, extent, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                   VK_SHARING_MODE_EXCLUSIVE, 0, NULL, caps.currentTransform,
                                   VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE,
                                   VK_NULL_HANDLE};
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR))
        sw.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (vkCreateSwapchainKHR(dev, &sw, NULL, &swapchain) != VK_SUCCESS)
        return -1;
    VkAttachmentDescription at = {0, format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                  VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref};
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &at, 1, &sp};
    vkCreateRenderPass(dev, &rpi, NULL, &pass);
    VkImage list[8];
    image_count = 8;
    vkGetSwapchainImagesKHR(dev, swapchain, &image_count, list);
    for (uint32_t k = 0; k < image_count; k++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, list[k], VK_IMAGE_VIEW_TYPE_2D,
                                    format, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCreateImageView(dev, &vi, NULL, &views[k]);
        VkFramebufferCreateInfo fbi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, pass, 1, &views[k],
                                       extent.width, extent.height, 1};
        vkCreateFramebuffer(dev, &fbi, NULL, &framebuffers[k]);
    }
    LOG("window %ux%u, %u images", extent.width, extent.height, image_count);
    return 0;
}

static void window_free(void)
{
    vkDeviceWaitIdle(dev);
    for (uint32_t k = 0; k < image_count; k++) {
        vkDestroyFramebuffer(dev, framebuffers[k], NULL);
        vkDestroyImageView(dev, views[k], NULL);
    }
    vkDestroyRenderPass(dev, pass, NULL);
    vkDestroySwapchainKHR(dev, swapchain, NULL);
    vkDestroySurfaceKHR(instance, surface, NULL);
    image_count = 0;
}

static void frame(void)
{
    vkWaitForFences(dev, 1, &done, VK_TRUE, UINT64_MAX);
    uint32_t k;
    if (vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &k) != VK_SUCCESS)
        return;
    vkResetFences(dev, 1, &done);
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &bi);
    VkClearValue clear = {{{preserves_float32 ? 0.0f : 0.8f, preserves_float32 ? 0.7f : 0.0f, 0.0f, 1.0f}}};
    if (touched) {
        clear.color.float32[0] += 0.2f;
        clear.color.float32[1] += 0.2f;
        clear.color.float32[2] = 0.8f;
    }
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, pass, framebuffers[k],
                                {{0, 0}, extent}, 1, &clear};
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &acquired, &wait, 1, &cmd, 1, &rendered};
    vkQueueSubmit(queue, 1, &si, done);
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &rendered, 1, &swapchain, &k};
    vkQueuePresentKHR(queue, &pi);
}

static void on_command(struct android_app* app, int32_t command)
{
    if (command == APP_CMD_INIT_WINDOW && app->window) {
        if (!dev && device_make())
            LOG("no Vulkan device");
        ready = dev && !window_make(app->window);
    } else if (command == APP_CMD_TERM_WINDOW && ready) {
        window_free();
        ready = 0;
    }
}

static int32_t on_input(struct android_app* app, AInputEvent* event)
{
    (void)app;
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
        return 0;
    const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    touched = action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_MOVE ||
              action == AMOTION_EVENT_ACTION_POINTER_DOWN;
    return 1;
}

void android_main(struct android_app* app)
{
    app->onAppCmd = on_command;
    app->onInputEvent = on_input;
    for (;;) {
        int events;
        struct android_poll_source* source;
        while (ALooper_pollOnce(ready ? 0 : -1, NULL, &events, (void**)&source) >= 0) {
            if (source)
                source->process(app, source);
            if (app->destroyRequested)
                return;
        }
        if (ready)
            frame();
    }
}
