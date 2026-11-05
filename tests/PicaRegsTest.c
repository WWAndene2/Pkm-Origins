/* src/platform/PicaRegs.c on command lists written by hand, one mechanism each (results worked out from Azahar's
 * ProcessCmdList and its register handlers):
 *   cc -O2 -I src/platform tests/PicaRegsTest.c src/platform/PicaRegs.c -o pica_regs_test && ./pica_regs_test */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "PicaRegs.h"

static int failed;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); failed = 1; } else printf("ok   %s\n", what); } while (0)

static uint32_t header(uint32_t id, uint32_t mask, uint32_t extra, int group)
{ return id | mask << 16 | extra << 20 | (uint32_t)group << 31; }

static int draws, indexed_draws;
static void on_draw(PicaState* s, int indexed, void* user) { (void)s; (void)user; draws++; indexed_draws += indexed; }

static uint32_t other[8];
static const uint32_t* memory(uint32_t physical, uint32_t bytes, void* user)
{
    (void)user;
    return physical == 0x18000000u && bytes == sizeof other ? other : NULL;
}

/* a float32 as float24's bits (exact for the values used: few mantissa bits) */
static uint32_t f24(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    if (!(u & 0x7FFFFFFF)) return (u >> 31) << 23;
    return (u >> 31) << 23 | (((u >> 23 & 0xFF) - 127 + 63) & 0x7F) << 16 | (u >> 7 & 0xFFFF);
}

int main(void)
{
    static PicaState s;
    pica_state_init(&s, on_draw, memory, NULL);

    /* a masked write: only bytes 0 and 2 of 0x0C0 */
    uint32_t a[] = {0xFFFFFFFF, header(0x0C0, 0xF, 0, 0), 0x11223344, header(0x0C0, 0x5, 0, 0)};
    pica_state_run(&s, a, 4);
    CHECK(s.reg[0x0C0] == 0xFF22FF44, "byte mask: bytes 0 and 2 written");

    /* consecutive registers: 0x0C1, 0x0C2, 0x0C3 from one header with two extra words (padding word after) */
    uint32_t b[] = {1, header(0x0C1, 0xF, 2, 1), 2, 3};
    pica_state_run(&s, b, 4);
    CHECK(s.reg[0x0C1] == 1 && s.reg[0x0C2] == 2 && s.reg[0x0C3] == 3, "group: consecutive registers");

    /* vertex shader code: offset 0, then three words to 0x2CC (one register written three times) */
    uint32_t c[] = {0, header(0x2CB, 0xF, 0, 0), 0xAAAA0001, header(0x2CC, 0xF, 2, 0), 0xAAAA0002, 0xAAAA0003};
    pica_state_run(&s, c, 6);
    CHECK(s.vs.code[0] == 0xAAAA0001 && s.vs.code[2] == 0xAAAA0003 && s.reg[0x2CB] == 3, "shader code upload");
    CHECK(s.gs.code[1] == 0xAAAA0002, "  copied to the geometry unit while it is not in use");

    /* a float24 uniform at c5: (x, y, z, w) = (1, -2, 0.5, 4), packed in three words */
    const uint32_t x = f24(1), y = f24(-2), z = f24(0.5f), w = f24(4);
    const uint32_t w0 = w << 8 | z >> 16, w1 = (z & 0xFFFF) << 16 | y >> 8, w2 = (y & 0xFF) << 24 | x;
    uint32_t d[] = {5, header(0x2C0, 0xF, 0, 0), w0, header(0x2C1, 0xF, 2, 0), w1, w2};
    pica_state_run(&s, d, 6);
    CHECK(s.vs.f[5][0] == 1 && s.vs.f[5][1] == -2 && s.vs.f[5][2] == 0.5f && s.vs.f[5][3] == 4, "float24 uniform");
    CHECK((s.reg[0x2C0] & 0x7F) == 6, "  the uniform index moved to c6");

    /* a float32 uniform at c6: four words, w first */
    float v32[4] = {7, 8, 9, 10};
    uint32_t u32[4];
    for (int k = 0; k < 4; k++) memcpy(&u32[k], &v32[3 - k], 4);
    uint32_t e[] = {6 | 0x80000000u, header(0x2C0, 0xF, 0, 0), u32[0], header(0x2C1, 0xF, 3, 0), u32[1], u32[2], u32[3], 0};
    pica_state_run(&s, e, 8);
    CHECK(s.vs.f[6][0] == 7 && s.vs.f[6][3] == 10, "float32 uniform");

    /* integer and boolean uniforms */
    uint32_t f[] = {0x00010203, header(0x2B1, 0xF, 0, 0), 0x8005, header(0x2B0, 0xF, 0, 0)};
    pica_state_run(&s, f, 4);
    CHECK(s.vs.i[0][0] == 3 && s.vs.i[0][2] == 1 && s.vs.b[0] && s.vs.b[2] && !s.vs.b[1] && s.vs.b[15], "int, bool uniforms");

    /* a jump into another buffer (its size and address in 8-byte units), which draws, then ends */
    other[0] = 1; other[1] = header(0x22F, 0xF, 0, 0); /* indexed draw */
    other[2] = 0x42; other[3] = header(0x0C4, 0xF, 0, 0);
    other[4] = 1; other[5] = header(0x22E, 0xF, 0, 0); /* draw */
    other[6] = 0; other[7] = header(0x000, 0, 0, 0);
    uint32_t g[] = {sizeof other / 8, header(0x238, 0xF, 0, 0), 0x18000000u / 8, header(0x23A, 0xF, 0, 0),
                    1, header(0x23C, 0xF, 0, 0), 0x99, header(0x0C5, 0xF, 0, 0)};
    pica_state_run(&s, g, 8);
    CHECK(draws == 2 && indexed_draws == 1, "jump: the other buffer's two draws");
    CHECK(s.reg[0x0C4] == 0x42 && s.reg[0x0C5] == 0, "  the jump does not come back");

    /* lighting table 3, from index 254: wraps within 256 */
    uint32_t h[] = {3 << 8 | 254, header(0x1C5, 0xF, 0, 0), 0x111, header(0x1C8, 0xF, 1, 0), 0x222, 0};
    pica_state_run(&s, h, 6);
    CHECK(s.lighting_lut[3][254] == 0x111 && s.lighting_lut[3][255] == 0x222 && (s.reg[0x1C5] & 0xFF) == 0, "lighting table");

    /* float24 decoding of the edges */
    CHECK(isinf(pica_float24(0x7F0000)) && isnan(pica_float24(0x7F0001)) && signbit(pica_float24(0x800000)),
          "float24: infinity, NaN, -0");
    CHECK(pica_float24(0x000001) > 0 && pica_float24(0x000001) < 1e-18f, "float24: a denormal kept, tiny");
    printf(failed ? "FAILED\n" : "all passed\n");
    return failed;
}
