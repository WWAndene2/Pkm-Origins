/* The PICA200's register state read from command words: the decoder of what the game's graphics library writes (a
 * model's BCH command blocks, the library's own lists), docs/RECOMPILATION_PLAN.md §11.2. The renderer reads the state
 * it leaves (materials, textures, vertex layouts) once, at load, and its draws; it never interprets lists per frame.
 *
 * Read as Azahar's PicaCore::ProcessCmdList and WriteInternalReg do (its frames are the reference): a value and a
 * header (register id 0-15, byte mask 16-19, extra words 20-27, consecutive registers bit 31), the list kept 8-byte
 * aligned between commands; the registers with side effects: shader code and operand descriptors (vertex 0x2CB-0x2DD,
 * geometry 0x29B-0x2AD), float uniforms (0x2C0-0x2C8, 0x290-0x298: three words a vec4 of float24, or four of float32),
 * integer and boolean uniforms, the vertex shader's writes copied to the geometry unit unless it is configured on its
 * own (0x244 bit 0) or in use (0x229 bits 0-1), lighting lookup tables (0x1C5, 0x1C8-0x1CF), the fog table (0x0E6,
 * 0x0E8-0x0EF), default vertex attributes (0x232-0x235), the command buffer jumps (0x238-0x23D) and the draws (0x22E,
 * 0x22F). Not kept: procedural texture tables, the interrupt (0x010). */
#ifndef PICA_REGS_H
#define PICA_REGS_H

#include <stdint.h>

#include "PicaShader.h"

enum { PICA_REGISTERS = 0x300 };

typedef struct PicaState PicaState;

struct PicaState {
    uint32_t reg[PICA_REGISTERS];
    PicaShaderSetup vs, gs;
    uint32_t lighting_lut[24][256];
    uint32_t fog_lut[128];
    float default_attribute[16][4];
    /* the packing queues of the float uniforms and default attributes */
    uint32_t vs_queue[4], gs_queue[4], attr_queue[3];
    int vs_queued, gs_queued, attr_queued;
    /* immediate mode (default attribute index 0xF, Azahar's SubmitImmediate): the vertex being formed, its attributes
       given so far, and whether the primitive assembly starts afresh (0x232, 0x25E or 0x25F written) */
    float immediate[16][4];
    int immediate_count, immediate_reset;
    /* changed whenever a word of the vertex shader's program or operand descriptors changes: the GPU vertex shader
       made from them is kept while it stays (VulkanRenderer.c) */
    uint32_t vs_version;
    /* the caller's: a draw (indexed 0 or 1; 2: an immediate-mode vertex formed, in state->immediate), with the state as
       it stands; the words of a command buffer the GPU jumps to
       (physical address and size in bytes), or NULL when not reachable */
    void (*draw)(PicaState* state, int indexed, void* user);
    const uint32_t* (*memory)(uint32_t physical, uint32_t bytes, void* user);
    void* user;
};

/* a clean state (all registers 0) with the caller's callbacks */
void pica_state_init(PicaState* state, void (*draw)(PicaState*, int, void*),
                     const uint32_t* (*memory)(uint32_t, uint32_t, void*), void* user);

/* runs command words (count of them) into the state; a jump moves to the buffer it names and goes on there */
void pica_state_run(PicaState* state, const uint32_t* words, uint32_t count);

/* a float24 (sign 23, exponent 16-22, mantissa 0-15) as float32, as Azahar's f24::FromRaw (denormals kept exact) */
float pica_float24(uint32_t raw);

#endif
