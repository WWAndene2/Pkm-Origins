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

/* at exit: the census, if asked */
void renderer_report(void);

#endif
