/* The renderer's Vulkan side, headless for now (docs/RECOMPILATION_PLAN.md §11.7, milestone 2: frames rendered to
 * image files and compared with Azahar's). Each draw: its triangles (PicaVertex.c), its fragment shader (PicaFragment.c,
 * compiled once per distinct shader and kept), its fixed state (blending or logic op, depth, stencil, colour mask,
 * culling, viewport, scissor) as a pipeline kept by key, its textures decoded once (PicaTexture.c) and kept by
 * address, size and format, drawn into a render target kept by its colour buffer's address.
 *
 * $HOS_RENDER_DIR=<dir> turns it on: the compiled shaders are kept there, and each display transfer (the game's copy of a
 * framebuffer to a screen) writes that render target as <dir>/frame_<n>_<address>.ppm.
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

#endif
