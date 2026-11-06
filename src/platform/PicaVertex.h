/* A draw's vertices, as the PICA200 makes them (docs/RECOMPILATION_PLAN.md §11, the first frames, option B): the
 * attributes read from the game's memory by the twelve loaders (0x200-0x227: formats, offsets, strides, the index
 * buffer), the vertex shader run on each (the interpreter, PicaShader.c: exact; the translated SPIR-V, checked equal to
 * it, comes with the GPU vertex path), its outputs placed by their semantics (0x04F-0x056), the triangles assembled
 * (lists, strips, fans: Azahar's PrimitiveAssembler, its winding kept). As Azahar's LoadVertices and
 * RasterizerAccelerated::AddTriangle: a 64-entry vertex cache for indexed draws (same results), each triangle's
 * second and third quaternions negated when opposite to the first's.
 *
 * The geometry shader (0x229 = 2): the vertex shader's outputs fed to it in Azahar's three modes (GeometryPipeline:
 * point, variable primitive, fixed primitive), its emit's triangles assembled with the winding it sets. Its unit keeps
 * its registers from one invocation to the next, as Azahar's. Not yet: vertex retention (Azahar has none either). */
#ifndef PICA_VERTEX_H
#define PICA_VERTEX_H

#include <stdint.h>

#include "PicaRegs.h"

/* one rasterizer vertex, Azahar's HardwareVertex: position, color, texcoord0, texcoord1, texcoord2, texcoord0_w,
   normquat, view (22 floats) */
typedef struct {
    float position[4], color[4], texcoord0[2], texcoord1[2], texcoord2[2], texcoord0_w, normquat[4], view[3];
} PicaVertex;

/* the draw's triangles (3 vertices each) into out (at most max vertices); the count of vertices written, or -1 when
   the memory the draw reads is not reachable */
long pica_vertex_draw(PicaState* state, int indexed, PicaVertex* out, long max);

/* an immediate-mode vertex (state->immediate, PicaRegs.c): the triangles it completes (0, or 3 vertices or more with a
   geometry shader) into out, the assembly kept between calls */
long pica_vertex_immediate(PicaState* state, PicaVertex* out, long max);

/* a draw's input attributes for the GPU's vertex shader: per vertex in draw order (the index buffer applied), its
   max_input_attrib_index + 1 attributes (0x2B9) as vec4s, packed; the vertex count, or -1 (memory not reachable,
   more than max vertices) */
long pica_vertex_inputs(PicaState* state, int indexed, float (*out)[4], long max);

#endif
