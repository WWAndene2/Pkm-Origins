/* The PICA200's fixed fragment stages as a GLSL fragment shader, generated from a draw's register state
 * (docs/RECOMPILATION_PLAN.md §11.2: one shader per material key, made at load). Covered: the six texture combiners
 * with their buffer, fragment lighting (8 lights, the lookup tables, bump and tangent maps, shadow factors,
 * clamp-highlights, geometric factors, Fresnel on the last light), the alpha test, the depth value (w-buffer or
 * z). Not yet: fog and gas (none on the title, Renderer.c's census), procedural textures, shadow rendering, cube and
 * projected textures, min / max blending (done in the pipeline's blend state otherwise).
 *
 * The formulas are those of Azahar's GLSL generator (video_core/shader/generator/glsl_fs_shader_gen.cpp: its frames
 * are the reference): results rounded to 1/255 after each combiner and at the end, the primary colour rounded first,
 * lookup tables sampled with linear interpolation of the entry and its difference.
 *
 * The interface: inputs at locations 0-6 (primary_color vec4, texcoord0-2 vec2, texcoord0_w float, normquat vec4, view
 * vec3), output color at location 0; set 0: binding 0 the uniform block PicaFragmentUniforms (std140, written by
 * pica_fragment_uniforms), binding 1 the lighting tables (a storage buffer of 24 x 256 vec2: value, difference),
 * bindings 2-4 the texture units' combined samplers. */
#ifndef PICA_FRAGMENT_H
#define PICA_FRAGMENT_H

#include <stddef.h>
#include <stdint.h>

#include "PicaRegs.h"

/* the uniform block, std140: every member a vec4 (or ivec4) */
typedef struct {
    float const_color[6][4];
    float buffer_color[4];
    float global_ambient[4];
    float blend_color[4];
    float depth[4];             /* scale, offset */
    int32_t alpha_ref[4];       /* x: the alpha test's reference, 0-255 */
    float tex_border[3][4];     /* the texture units' border colours */
    float tex_lod_bias[4];      /* their level-of-detail biases (x, y, z: units 0, 1, 2) */
    struct {
        float specular_0[4], specular_1[4], diffuse[4], ambient[4], position[4], spot_direction[4];
        float atten[4];         /* x: distance attenuation bias, y: scale */
    } light[8];
} PicaFragmentUniforms;

/* the GLSL of the draw's fragment stages (with #version and the float controls); its length, or -1 if out is short */
long pica_fragment_glsl(const PicaState* state, char* out, size_t size);

/* the uniform values of the draw */
void pica_fragment_uniforms(const PicaState* state, PicaFragmentUniforms* u);

/* the lighting tables as the shader reads them: 24 x 256 (value, difference) */
void pica_fragment_luts(const PicaState* state, float out[24 * 256][2]);

/* a diagnostic: the alpha test left out when set */
extern int pica_fragment_no_alpha_test;

/* half and 20-bit floats of the PICA200 (Azahar's f16, f20) */
float pica_float16(uint32_t raw);
float pica_float20(uint32_t raw);

#endif
