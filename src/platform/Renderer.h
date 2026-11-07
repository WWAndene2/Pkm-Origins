/* The renderer's entry from the 3DS system layer (src/hos/service/Gsp.c): the command lists the game's graphics library
 * submits, decoded by PicaRegs.c (docs/RECOMPILATION_PLAN.md §11, the owner's option B of 5 November: the first frames
 * from the lists the library builds, the entry then moved up into the library function by function, each step checked
 * against the frame before; this entry goes once the library is plugged in).
 *
 * For now: $HOS_DRAW_STATS=<file> writes, at exit, what the game's draws ask of the renderer (a census: primitive kinds,
 * texture formats, combiner modes, fragment lighting, fog, alpha test, blending, depth, geometry shader, framebuffers),
 * the input for what to build first. */
#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>

/* a command list at virtual address address, size bytes (GSP's SubmitCmdList) */
void renderer_submit_list(uint32_t address, uint32_t size);

/* a display transfer (GSP's copy of a framebuffer to a screen) from virtual address source */
void renderer_display(uint32_t source);

/* GSP's memory fill of virtual start-end with value (width_bits as its control's bits 8-9) */
void renderer_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width_bits);
/* the game's memory written by a copy of the GSP (a texture copy): from start, bytes long (game addresses) */
void renderer_copied(uint32_t start, uint32_t bytes);

/* the GPU's work on game memory start-start + bytes (game addresses) written into that memory before something reads it
   (a display transfer: Azahar's FlushRegion) */
void renderer_flush(uint32_t start, uint32_t bytes);

/* at exit: the census, if asked */
void renderer_report(void);

#endif
