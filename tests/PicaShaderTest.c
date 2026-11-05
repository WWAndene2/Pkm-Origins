/* src/platform/PicaShader.c on small programs written by hand, each result worked out from Azahar's semantics:
 *   cc -O2 -I src/platform tests/PicaShaderTest.c src/platform/PicaShader.c -lm -o pica_shader_test && ./pica_shader_test */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "PicaShader.h"

static int failed;
#define CHECK(cond, what) do { if (!(cond)) { printf("FAIL %s\n", what); failed = 1; } else printf("ok   %s\n", what); } while (0)

/* identity selectors (x y z w) for src1, src2, src3, all four components written */
#define XYZW 0x1Bu
#define DESC_ALL (0xFu | XYZW << 5 | XYZW << 14 | XYZW << 23)
static uint32_t arith(uint32_t op, uint32_t dest, uint32_t src1, uint32_t src2, uint32_t desc)
{ return op << 26 | dest << 21 | src1 << 12 | src2 << 7 | desc; }
static uint32_t flow(uint32_t op, uint32_t target, uint32_t count, uint32_t id)
{ return op << 26 | id << 22 | target << 10 | count; }

int main(void)
{
    static PicaShaderSetup s;
    PicaShaderUnit u;

    /* mov o0, v0; mul o1, v0, c0; dp4 o2, v0, c0; end */
    memset(&s, 0, sizeof s);
    s.swizzle[0] = DESC_ALL;
    s.code[0] = arith(0x13, 0, 0x00, 0, 0);
    s.code[1] = arith(0x08, 1, 0x20, 0, 0); /* (src1 = c0, src2 = v0) */
    s.code[2] = arith(0x02, 2, 0x20, 0, 0);
    s.code[3] = 0x22u << 26;
    memset(&u, 0, sizeof u);
    float v[4] = {1, 2, 3, 4}, c[4] = {0.5f, -1, 2, INFINITY};
    memcpy(u.input[0], v, sizeof v);
    memcpy(s.f[0], c, sizeof c);
    CHECK(pica_shader_run(&s, &u, 0, 1000) == 0, "the program ends");
    CHECK(!memcmp(u.output[0], v, sizeof v), "mov copies");
    CHECK(u.output[1][0] == 0.5f && u.output[1][1] == -2 && u.output[1][2] == 6 && isinf(u.output[1][3]), "mul");
    CHECK(isinf(u.output[2][0]), "dp4 with an infinity");

    /* 0 x infinity = 0 (Azahar's float24 product) */
    memset(&u, 0, sizeof u);
    u.input[0][3] = 0;
    CHECK(pica_shader_run(&s, &u, 0, 1000) == 0 && u.output[1][3] == 0, "0 x infinity is 0");

    /* loop i0 (x = 3: four passes, y = 0 start, z = 1 step) over add r0, r0, v0; then mov o0, r0 */
    memset(&s, 0, sizeof s);
    s.swizzle[0] = DESC_ALL;
    s.i[0][0] = 3; s.i[0][1] = 0; s.i[0][2] = 1;
    s.code[0] = flow(0x29, 1, 0, 0);            /* loop: body up to and including word 1 */
    s.code[1] = arith(0x00, 0x10, 0x10, 0, 0);   /* add r0, r0, v0 */
    s.code[2] = arith(0x13, 0, 0x10, 0, 0);      /* mov o0, r0 */
    s.code[3] = 0x22u << 26;
    memset(&u, 0, sizeof u);
    memcpy(u.input[0], v, sizeof v);
    CHECK(pica_shader_run(&s, &u, 0, 1000) == 0 && u.output[0][0] == 4 && u.output[0][3] == 16, "loop runs x + 1 times");
    CHECK(u.address[2] == 4, "aL stepped once a pass");

    /* ifu b0: if true take words 1-1, else 2-2 (target 2, count 1) */
    memset(&s, 0, sizeof s);
    s.swizzle[0] = DESC_ALL;
    s.code[0] = flow(0x27, 2, 1, 0);
    s.code[1] = arith(0x13, 0, 0x20, 0, 0);       /* mov o0, c0 (then: jumps past the else) */
    s.code[2] = arith(0x13, 0, 0x21, 0, 0);       /* mov o0, c1 (the else) */
    s.code[3] = 0x22u << 26;
    s.f[0][0] = 10; s.f[1][0] = 20;
    for (int b = 0; b < 2; b++) {
        s.b[0] = b;
        memset(&u, 0, sizeof u);
        pica_shader_run(&s, &u, 0, 1000);
        CHECK(u.output[0][0] == (b ? 10 : 20), b ? "ifu true: the then part only" : "ifu false: the else part");
    }
    printf(failed ? "FAILED\n" : "all passed\n");
    return failed;
}
