/* The renderer's Vulkan side, headless for now (docs/RECOMPILATION_PLAN.md §11.7, milestone 2: frames rendered to
 * image files and compared with Azahar's). Each draw: its triangles (PicaVertex.c), its fragment shader (PicaFragment.c,
 * compiled once per distinct shader and kept), its fixed state (blending or logic op, depth, stencil, colour mask,
 * culling, viewport, scissor) as a pipeline kept by key, its textures decoded (PicaTexture.c) and kept by
 * address, size and format (decoded again when their bytes change, a render target's own image when they are one), drawn into a render target kept by its colour buffer's address.
 *
 * $HOS_RENDER_DIR=<dir> turns it on: the compiled shaders are kept there, and each display transfer (the game's copy of a
 * framebuffer to a screen) writes that render target as <dir>/frame_<n>_<address>.ppm. $HOS_TEXTURE_DIR=<dir> also
 * writes each texture as it is decoded (tex_<frame>_<address>_f<format>_<w>x<h>.ppm).
 *
 * First form, to be made fast after it is right: one submission and wait per draw, the vertex shader on the CPU, the
 * shaders compiled by running glslangValidator, textures decoded to RGBA8 (ETC2 waits for a device that has it:
 * lavapipe has not). */
#ifndef VULKAN_RENDERER_H
#define VULKAN_RENDERER_H

#include <stdint.h>

#include "PicaRegs.h"

/* 0 when ready (Vulkan device found, $HOS_RENDER_DIR set), -1 otherwise */
int vk_renderer_init(void);
void vk_renderer_draw(PicaState* state, int indexed);
/* a display transfer from physical address source: the frame of that render target written out */
void vk_renderer_display(uint32_t source);

/* a memory fill (GSP's) of physical start-end with value (width_bits: 0 16-bit, 1 24-bit, 2 32-bit): a render target or
   its depth buffer cleared when the fill covers it */
void vk_renderer_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width_bits);

/* the render targets overlapping physical address-address + bytes written into the game's memory (host gives the
   memory at a physical address), before something reads that memory (a display transfer) */
void vk_renderer_flush(uint32_t address, uint32_t bytes, uint8_t* (*host)(uint32_t physical, uint32_t bytes));

/* the game's memory written by a copy (physical address, bytes): a render target it falls in is told (counted; its
   image is not yet loaded back from memory) */
void vk_renderer_copied(uint32_t address, uint32_t bytes);

#endif
