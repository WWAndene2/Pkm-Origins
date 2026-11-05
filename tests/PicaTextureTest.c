/* src/platform/PicaTexture.c checked:
 * - the ETC1 -> ETC2 transcode: random ETC1 textures (no overflowing differential block) transcoded, then decoded by an
 *   ETC2 decoder written here from the Khronos Data Format specification (ETC2's individual and differential modes,
 *   which ETC1 data is), pixel for pixel against the decoding of the original as Azahar does it;
 * - a texture with an overflowing differential block refused (decoded to RGBA8 instead);
 * - texels of the other formats worked out by hand from Azahar's decoders (Morton order, bit expansions).
 *   cc -O2 -I src/platform tests/PicaTextureTest.c src/platform/PicaTexture.c -o pica_texture_test && ./pica_texture_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PicaTexture.h"

static int failed;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); failed = 1; } else printf("ok   %s\n", what); } while (0)

static uint32_t rng = 99;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* ETC2 RGB8, individual and differential modes only, from the specification: block bytes big-endian */
static void etc2_pixel(const uint8_t* b, int x, int y, uint8_t rgb[3])
{
    static const int mods[8][4] = {{2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
                                   {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}};
    const int diff = b[3] >> 1 & 1, flip = b[3] & 1;
    const int second = flip ? y >= 2 : x >= 2;
    int base[3];
    for (int k = 0; k < 3; k++) {
        if (diff) {
            int c = b[k] >> 3, d = b[k] & 7;
            if (d >= 4) d -= 8;
            if (second) c += d;
            base[k] = c << 3 | c >> 2;
        } else {
            int c = second ? b[k] & 15 : b[k] >> 4;
            base[k] = c * 17;
        }
    }
    const int cw = second ? b[3] >> 2 & 7 : b[3] >> 5;
    const int i = x * 4 + y;
    const int msb = ((b[4] << 8 | b[5]) >> i) & 1, lsb = ((b[6] << 8 | b[7]) >> i) & 1;
    const int m = mods[cw][msb << 1 | lsb];
    for (int k = 0; k < 3; k++) {
        int v = base[k] + m;
        rgb[k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

int main(void)
{
    enum { W = 64, H = 32 };
    static uint8_t etc1[W * H / 2], etc2[W * H / 2], rgba[W * H * 4];
    int textures = 0;
    for (int t = 0; t < 200; t++) {
        for (size_t k = 0; k < sizeof etc1; k++)
            etc1[k] = (uint8_t)rnd();
        /* no overflow: each differential block's channels kept so that base + delta stays in 0-31 */
        for (size_t blk = 0; blk < sizeof etc1 / 8; blk++) {
            uint8_t* b = etc1 + blk * 8; /* little-endian: byte 7 holds r (bits 59-63) and dr (56-58) */
            if (!(b[4] >> 1 & 1))
                continue;
            for (int k = 0; k < 3; k++) {
                uint8_t* byte = b + 7 - k;
                int c = *byte >> 3, d = *byte & 7;
                if (d >= 4) d -= 8;
                if (c + d < 0 || c + d > 31)
                    *byte = (uint8_t)(16 << 3 | (*byte & 7));
            }
        }
        if (pica_texture_etc1_to_etc2(etc1, W, H, etc2)) {
            printf("FAIL texture %d refused\n", t);
            failed = 1;
            continue;
        }
        pica_texture_decode(PICA_ETC1, etc1, W, H, rgba);
        int bad = 0;
        for (int y = 0; y < H && !bad; y++)
            for (int x = 0; x < W && !bad; x++) {
                uint8_t c[3];
                etc2_pixel(etc2 + ((y / 4) * (W / 4) + x / 4) * 8, x % 4, y % 4, c);
                if (memcmp(c, rgba + (y * W + x) * 4, 3)) {
                    printf("FAIL texture %d at (%d, %d): ETC2 %d %d %d, Azahar's ETC1 %d %d %d\n", t, x, y, c[0], c[1],
                           c[2], rgba[(y * W + x) * 4], rgba[(y * W + x) * 4 + 1], rgba[(y * W + x) * 4 + 2]);
                    bad = failed = 1;
                }
            }
        textures += !bad;
    }
    printf("%s ETC1 -> ETC2: %d of 200 random textures identical\n", textures == 200 ? "ok  " : "FAIL", textures);

    /* an overflowing block: r = 31, dr = +1, differential on */
    memset(etc1, 0, sizeof etc1);
    etc1[4] = 2;              /* bit 33: differential */
    etc1[7] = 31 << 3 | 1;    /* r 31, dr +1 */
    CHECK(pica_texture_etc1_to_etc2(etc1, W, H, etc2) == -1, "an overflowing differential block is refused");

    /* the other formats by hand: texel (1, 0) is Morton 1, (0, 1) is 2, (2, 0) is 4; (8, 0) starts the next tile */
    uint8_t px[4], tex[256 * 4 * 2];
    memset(tex, 0, sizeof tex);
    const uint16_t rgb565 = 31 << 11 | 0 << 5 | 16; /* r 31, g 0, b 16 */
    memcpy(tex + 1 * 2, &rgb565, 2);
    pica_texture_texel(PICA_RGB565, tex, 16, 1, 0, px);
    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 132 && px[3] == 255, "RGB565 at Morton 1 (b 16 -> 132)");
    memset(tex, 0, sizeof tex);
    tex[2 * 4 + 0] = 0x40; tex[2 * 4 + 1] = 0x30; tex[2 * 4 + 2] = 0x20; tex[2 * 4 + 3] = 0x10; /* ABGR in memory */
    pica_texture_texel(PICA_RGBA8, tex, 16, 0, 1, px);
    CHECK(px[0] == 0x10 && px[1] == 0x20 && px[2] == 0x30 && px[3] == 0x40, "RGBA8 at (0, 1): bytes ABGR");
    memset(tex, 0, sizeof tex);
    tex[64 + 0] = 0x9C; /* the second tile's first texel: (8, 0) */
    pica_texture_texel(PICA_IA4, tex, 16, 8, 0, px);
    CHECK(px[0] == 0x99 && px[3] == 0xCC, "IA4 in the second tile: i 9 -> 0x99, a C -> 0xCC");
    memset(tex, 0, sizeof tex);
    tex[2] = 0xA5; /* texels 4 (low nibble) and 5 (high) */
    pica_texture_texel(PICA_I4, tex, 16, 2, 0, px);
    uint8_t px2[4];
    pica_texture_texel(PICA_I4, tex, 16, 3, 0, px2);
    CHECK(px[0] == 0x55 && px2[0] == 0xAA, "I4: Morton 4 the low nibble, 5 the high");
    const uint16_t rgb5a1 = 1 << 11 | 31 << 6 | 0 << 1 | 1;
    memset(tex, 0, sizeof tex);
    memcpy(tex, &rgb5a1, 2);
    pica_texture_texel(PICA_RGB5A1, tex, 16, 0, 0, px);
    CHECK(px[0] == 8 && px[1] == 255 && px[2] == 0 && px[3] == 255, "RGB5A1 (r 1 -> 8, a 1 -> 255)");
    printf(failed ? "FAILED\n" : "all passed\n");
    return failed;
}
