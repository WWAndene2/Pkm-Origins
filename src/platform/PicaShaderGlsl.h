/* The PICA200 shader translator, first form: a program to GLSL (then SPIR-V by glslang), docs/RECOMPILATION_PLAN.md
 * §11.2. The GLSL is the interpreter (PicaShader.c) written out for one program: a loop over a switch on the program
 * counter, one case per instruction, the same stacks and the same pop order, so that any program (jumps, calls, ifs,
 * loops, breaks, Azahar's four-call bug included) translates the same way the interpreter runs it. Structured code
 * (no switch) is a later, faster form, checked against this one.
 *
 * The values: float24 as float32, every result `precise` (no fused multiply-add, no reordering), the product as
 * Azahar's (0 x infinity = 0). Exact against the interpreter for every instruction but rcp, rsq, ex2 and lg2, which
 * the GPU computes to its own precision (Vulkan's: rcp and rsq within 2.5 / 2 ULP, ex2 and lg2 within 3 ULP and an
 * absolute 2^-21 near 1 for lg2); the check (tests/PicaShaderGlslTest.c) allows that much there and nothing elsewhere.
 *
 * The interface: the inputs v0-v15 and outputs o0-o15 as arrays the caller declares (`vec4 pica_v[16]`,
 * `vec4 pica_o[16]`), the uniforms as `vec4 pica_c[96]`, `ivec4 pica_i[4]`, `int pica_b[16]` (a vertex shader declares
 * them as attributes, outputs and a uniform block; the check's compute shader as storage buffers). The function is
 * `void pica_main()`.
 *
 * Not translated as the interpreter runs it: a program that runs past its last non-zero word without an end (the
 * interpreter would go on through the zero words, each an add); the GLSL stops there. */
#ifndef PICA_SHADER_GLSL_H
#define PICA_SHADER_GLSL_H

#include <stddef.h>
#include <stdint.h>

#include "PicaShader.h"

/* What a shader using the translation puts after its #version line: SPIR-V's SignedZeroInfNanPreserve for 32-bit
   floats (VK_KHR_shader_float_controls, core in Vulkan 1.2), without which a driver may treat -0 as 0 and assume no NaN
   or infinity (lavapipe does, and the 0 x infinity rule then breaks). A device without
   shaderSignedZeroInfNanPreserveFloat32 cannot run the translation exactly: to check on the owner's phones. */
#define PICA_SHADER_GLSL_FLOAT_CONTROLS \
    "#extension GL_EXT_spirv_intrinsics : require\n" \
    "spirv_execution_mode(extensions = [\"SPV_KHR_float_controls\"], capabilities = [4466], 4461, 32);\n"

/* Writes the GLSL body of the program (code and operand descriptors of setup, from entry) into out (size bytes);
   returns its length, or -1 when out is too small. max_steps bounds the instructions run, as the interpreter's. */
long pica_shader_glsl(const PicaShaderSetup* setup, uint32_t entry, uint32_t max_steps, char* out, size_t size);

#endif
