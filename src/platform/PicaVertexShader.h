/* The PICA200's vertex shader as a Vulkan vertex shader (docs/RECOMPILATION_PLAN.md §11.2, speed: the shader on the
 * GPU instead of the interpreter on the CPU). The program's body is PicaShaderGlsl.c's translation (exact against the
 * interpreter but for rcp, rsq, ex2, lg2 to the GPU's precision); around it, the draw's input attributes (location k:
 * the k-th attribute, mapped to the shader's input registers by 0x2BB-0x2BC), the uniforms, and the outputs placed by
 * their semantics (0x04F-0x056) into PicaFragment.c's inputs, as PicaVertex.c's to_vertex does on the CPU; the
 * position as Azahar's: y negated when the framebuffer is flipped, -z for Vulkan's depth range.
 *
 * The interface: inputs at locations 0..max_input_attrib_index (vec4), the uniform block at set 0 binding 5
 * (PicaVertexUniforms, std140), outputs at locations 0-6 as PicaFragment.h's inputs. Not on this path (PicaVertex.c's
 * CPU path keeps them): the geometry shader, immediate mode, and draws whose fragment lighting reads the quaternion
 * (made per triangle on the CPU, as Azahar's software path does: a GPU needs barycentrics or a geometry stage). */
#ifndef PICA_VERTEX_SHADER_H
#define PICA_VERTEX_SHADER_H

#include <stddef.h>
#include <stdint.h>

#include "PicaRegs.h"

/* the uniform block, std140 */
typedef struct {
    float c[96][4];
    int32_t i[4][4];
    int32_t b[4][4];   /* the 16 booleans, four to an ivec4 */
    float flip[4];     /* x: -1 when the framebuffer is flipped (y negated), else 1 */
} PicaVertexUniforms;

/* the GLSL of the draw's vertex shader (with #version and the float controls); its length, or -1 if out is short */
long pica_vertex_shader_glsl(const PicaState* state, char* out, size_t size);

/* the draw's uniform values */
void pica_vertex_shader_uniforms(const PicaState* state, PicaVertexUniforms* u);

/* a key for the shader of the draw's state: the program's version and the registers the GLSL reads */
uint64_t pica_vertex_shader_key(const PicaState* state);

#endif
