/* The PICA200 texture formats: see PicaTexture.h. Each case is Azahar's LookupTexelInTile for the same format. */
#include "PicaTexture.h"

#include <string.h>

static uint8_t c1(uint8_t v) { return (uint8_t)(v * 255); }
static uint8_t c4(uint8_t v) { return (uint8_t)(v << 4 | v); }
static uint8_t c5(uint8_t v) { return (uint8_t)(v << 3 | v >> 2); }
static uint8_t c6(uint8_t v) { return (uint8_t)(v << 2 | v >> 4); }

static uint32_t morton(uint32_t x, uint32_t y)
{
    static const uint32_t xl[] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const uint32_t yl[] = {0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a};
    return xl[x % 8] + yl[y % 8];
}

static size_t tile_size(uint32_t format)
{
    switch (format) {
    case PICA_RGBA8: return 4 * 64;
    case PICA_RGB8: return 3 * 64;
    case PICA_RGB5A1: case PICA_RGB565: case PICA_RGBA4: case PICA_IA8: case PICA_RG8: return 2 * 64;
    case PICA_I8: case PICA_A8: case PICA_IA4: return 64;
    case PICA_I4: case PICA_A4: return 32;
    case PICA_ETC1: return 4 * 8;
    case PICA_ETC1A4: return 4 * 16;
    default: return 0;
    }
}

size_t pica_texture_size(uint32_t format, uint32_t width, uint32_t height)
{
    return tile_size(format) * (width / 8) * (height / 8);
}

static uint64_t le64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int k = 7; k >= 0; k--)
        v = v << 8 | p[k];
    return v;
}

static int sext3(uint64_t v) { return (int)(v & 3) - (int)(v & 4); }

/* Azahar's ETC1Tile::GetRGB: (x, y) inside the 4x4 block */
static void etc1_rgb(uint64_t t, uint32_t x, uint32_t y, uint8_t rgb[3])
{
    static const uint8_t mods[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
    const unsigned texel = 4 * x + y;
    if (t >> 32 & 1) { uint32_t s = x; x = y; y = s; }
    int c[3];
    if (t >> 33 & 1) {
        const int base[3] = {(int)(t >> 59 & 31), (int)(t >> 51 & 31), (int)(t >> 43 & 31)};
        const int delta[3] = {sext3(t >> 56), sext3(t >> 48), sext3(t >> 40)};
        for (int k = 0; k < 3; k++)
            c[k] = c5((uint8_t)(base[k] + (x >= 2 ? delta[k] : 0))); /* (as Azahar: a value out of 0-31 wraps in u8) */
    } else {
        const int shift[3] = {x < 2 ? 60 : 56, x < 2 ? 52 : 48, x < 2 ? 44 : 40};
        for (int k = 0; k < 3; k++)
            c[k] = c4((uint8_t)(t >> shift[k] & 15));
    }
    const unsigned table = x < 2 ? (unsigned)(t >> 37 & 7) : (unsigned)(t >> 34 & 7);
    int m = mods[table][t >> texel & 1];
    if (t >> (16 + texel) & 1)
        m = -m;
    for (int k = 0; k < 3; k++) {
        int v = c[k] + m;
        rgb[k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

void pica_texture_texel(uint32_t format, const uint8_t* data, uint32_t width, uint32_t x, uint32_t y, uint8_t o[4])
{
    const uint8_t* tile = data + (y / 8) * tile_size(format) * (width / 8) + (x / 8) * tile_size(format);
    const uint32_t fx = x % 8, fy = y % 8, m = morton(fx, fy);
    const uint8_t* p;
    uint16_t v;
    switch (format) {
    case PICA_RGBA8: p = tile + m * 4; o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; o[3] = p[0]; return;
    case PICA_RGB8: p = tile + m * 3; o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; return;
    case PICA_RGB5A1:
        memcpy(&v, tile + m * 2, 2);
        o[0] = c5(v >> 11 & 31); o[1] = c5(v >> 6 & 31); o[2] = c5(v >> 1 & 31); o[3] = c1(v & 1);
        return;
    case PICA_RGB565:
        memcpy(&v, tile + m * 2, 2);
        o[0] = c5(v >> 11 & 31); o[1] = c6(v >> 5 & 63); o[2] = c5(v & 31); o[3] = 255;
        return;
    case PICA_RGBA4:
        memcpy(&v, tile + m * 2, 2);
        o[0] = c4(v >> 12 & 15); o[1] = c4(v >> 8 & 15); o[2] = c4(v >> 4 & 15); o[3] = c4(v & 15);
        return;
    case PICA_IA8: p = tile + m * 2; o[0] = o[1] = o[2] = p[1]; o[3] = p[0]; return;
    case PICA_RG8: p = tile + m * 2; o[0] = p[1]; o[1] = p[0]; o[2] = 0; o[3] = 255; return;
    case PICA_I8: o[0] = o[1] = o[2] = tile[m]; o[3] = 255; return;
    case PICA_A8: o[0] = o[1] = o[2] = 0; o[3] = tile[m]; return;
    case PICA_IA4: o[0] = o[1] = o[2] = c4(tile[m] >> 4); o[3] = c4(tile[m] & 15); return;
    case PICA_I4: { uint8_t i = c4(m % 2 ? tile[m / 2] >> 4 : tile[m / 2] & 15); o[0] = o[1] = o[2] = i; o[3] = 255; return; }
    case PICA_A4: { uint8_t a = c4(m % 2 ? tile[m / 2] >> 4 : tile[m / 2] & 15); o[0] = o[1] = o[2] = 0; o[3] = a; return; }
    case PICA_ETC1: case PICA_ETC1A4: {
        const int alpha = format == PICA_ETC1A4;
        const uint8_t* sub = tile + (fx / 4 + 2 * (fy / 4)) * (alpha ? 16 : 8);
        const uint32_t sx = fx % 4, sy = fy % 4;
        o[3] = 255;
        if (alpha) {
            o[3] = c4((uint8_t)(le64(sub) >> (4 * (sx * 4 + sy)) & 15));
            sub += 8;
        }
        etc1_rgb(le64(sub), sx, sy, o);
        return;
    }
    default: o[0] = o[1] = o[2] = o[3] = 0; return;
    }
}

void pica_texture_decode(uint32_t format, const uint8_t* data, uint32_t width, uint32_t height, uint8_t* out)
{
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
            pica_texture_texel(format, data, width, x, y, out + (y * width + x) * 4);
}

int pica_texture_etc1_to_etc2(const uint8_t* data, uint32_t width, uint32_t height, uint8_t* out)
{
    const size_t stride = 32 * (width / 8);
    for (uint32_t by = 0; by < height / 4; by++)
        for (uint32_t bx = 0; bx < width / 4; bx++) {
            const uint8_t* sub = data + (by / 2) * stride + (bx / 2) * 32 + ((bx % 2) + 2 * (by % 2)) * 8;
            const uint64_t t = le64(sub);
            if (t >> 33 & 1) {
                const int base[3] = {(int)(t >> 59 & 31), (int)(t >> 51 & 31), (int)(t >> 43 & 31)};
                const int delta[3] = {sext3(t >> 56), sext3(t >> 48), sext3(t >> 40)};
                for (int k = 0; k < 3; k++)
                    if (base[k] + delta[k] < 0 || base[k] + delta[k] > 31)
                        return -1;
            }
            uint8_t* o = out + ((size_t)by * (width / 4) + bx) * 8;
            for (int k = 0; k < 8; k++)
                o[k] = (uint8_t)(t >> (56 - 8 * k));
        }
    return 0;
}
