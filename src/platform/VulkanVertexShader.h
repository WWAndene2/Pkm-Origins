/* The vertex shader on the GPU (docs/RECOMPILATION_PLAN.md §11.3, the next step of 6 November: the CPU's interpreter
 * was the renderer's largest cost). A draw's vertices are made in two compute passes, recorded into a command buffer
 * of their own that is submitted before the frame's drawing (no render pass is broken for them):
 *
 *   1. the PICA program, translated to GLSL (PicaShaderGlsl.c, checked equal to the interpreter: its unrolled form for
 *      the draw's boolean and integer uniforms, else the switch form), run
 *      once for each distinct vertex the draw uses; its inputs are the loaders' attributes, read on the CPU as before
 *      (PicaVertex.c's pica_loader_read) and placed by the input map; its outputs placed by their semantics into a
 *      rasterizer vertex (PicaVertex), the y negated for a flipped framebuffer;
 *   2. the triangles assembled as PicaVertex.c's assembler does (lists, strips with their alternating order, fans),
 *      each triangle's second and third quaternions negated when opposite to the first's, written where the CPU path
 *      writes its triangles (the renderer's vertex ring): the drawing that follows is the same.
 *
 * Not handled here (vk_vertex_record says so, and the CPU path makes them): immediate mode, the geometry shader.
 * $HOS_VERTEX_CPU=1 keeps every draw on the CPU (to compare the two).
 *
 * The values are the interpreter's but for rcp, rsq, ex2 and lg2, which the GPU computes to its own precision
 * (PicaShaderGlsl.h); a vertex's results do not depend on the 64-entry vertex cache (it only saves work). */
#ifndef VULKAN_VERTEX_SHADER_H
#define VULKAN_VERTEX_SHADER_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "PicaRegs.h"

/* what the module needs from the renderer */
typedef struct {
    VkDevice dev;
    VkDeviceSize ssbo_align;
    /* a host-visible buffer mapped (0, or -1) */
    int (*make_buffer)(VkBuffer* buf, VkDeviceMemory* mem, void** map, VkDeviceSize size, VkBufferUsageFlags usage);
    /* a GLSL shader compiled (kept by its text's hash), or VK_NULL_HANDLE */
    VkShaderModule (*compile)(const char* glsl, size_t len, const char* stage);
} VkVertexContext;

enum { VK_VERTEX_CPU = -2, VK_VERTEX_FULL = -3 };

/* 0 when ready */
int vk_vertex_init(const VkVertexContext* context);

/* whether the draw's vertices are made here (the module on, not immediate mode, no geometry shader) */
int vk_vertex_handles(const PicaState* state, int indexed);

/* The draw's triangles recorded into cmd (a compute command buffer, open), written into out (a storage buffer) from
 * byte offset out_offset, at most max vertices. Returns the vertices that will be written (3 a triangle, 0 for none),
 * -1 when the draw's memory is not reachable, VK_VERTEX_CPU when the CPU path must make this draw, VK_VERTEX_FULL when
 * the module's buffers are full (submit the work recorded, call vk_vertex_reset, try again). */
long vk_vertex_record(PicaState* state, int indexed, VkCommandBuffer cmd, VkBuffer out, VkDeviceSize out_offset, long max);

/* the work recorded so far has been submitted and finished: the module's buffers free again */
void vk_vertex_reset(void);

#endif
