/* The PICA200's vertex (and geometry) shader units, run instruction by instruction: the reference the shader
 * translator (PICA200 code to SPIR-V, docs/RECOMPILATION_PLAN.md §11.2) is checked against, as the translated ARM code
 * is checked against Unicorn.
 *
 * The semantics are Azahar's interpreter's (src/video_core/shader/shader_interpreter.cpp, the frames compared against
 * are Azahar's, §11.6): float24 values held as float32 (not rounded to 24 bits), a product that comes out NaN from two
 * non-NaN inputs (0 x infinity) is 0, mad is a product then a sum (no fused multiply-add), dot products sum from 0 in
 * order, max / min take the second operand unless the first compares greater / less (NaN semantics), stacks of 8 ifs,
 * 4 calls, 4 loops with Azahar's pop order and its four-call hardware bug, a float uniform indexed by an address
 * register past 95 reads as ones. The geometry shader's setemit records the vertex slot, primitive and winding flags,
 * emit hands the outputs to the unit's emit callback and switches the output bank (Azahar's GeometryEmitter: the writes
 * after an emit go to the other bank, which keeps what was written there before). Not run: lit and dst (Azahar has none).
 */
#ifndef PICA_SHADER_H
#define PICA_SHADER_H

#include <stdint.h>

enum { PICA_PROGRAM_WORDS = 4096, PICA_SWIZZLES = 4096 };

typedef struct {
    uint32_t code[PICA_PROGRAM_WORDS];    /* the program (GPUREG_VSH_CODETRANSFER_DATA) */
    uint32_t swizzle[PICA_SWIZZLES];      /* the operand descriptors (GPUREG_VSH_OPDESCS_DATA) */
    float f[96][4];                       /* float uniforms c0-c95 */
    uint8_t i[4][4];                      /* integer uniforms i0-i3: x count, y start, z step */
    uint8_t b[16];                        /* boolean uniforms b0-b15 */
} PicaShaderSetup;

typedef struct PicaShaderUnitTag {
    float input[16][4];
    float temp[16][4];
    float output[16][4];
    int32_t address[3];                   /* a0.x, a0.y, aL */
    uint8_t cond[2];                      /* the conditional codes */
    uint32_t steps;                       /* instructions run (a bound for broken programs) */
    /* the geometry shader's: the other output bank, setemit's state, the callback emit calls (none: emit does nothing) */
    float other_bank[16][4];
    uint8_t emit_vertex, emit_primitive, emit_winding;
    void (*emit)(void* context, struct PicaShaderUnitTag* unit);
    void* emit_context;
} PicaShaderUnit;

/* Runs the program from entry until end (or the last instruction, or max_steps instructions); 0 when it ended, -1 when
   stopped by max_steps. */
int pica_shader_run(const PicaShaderSetup* setup, PicaShaderUnit* unit, uint32_t entry, uint32_t max_steps);

#endif
