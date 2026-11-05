/* The PICA200's texture formats, decoded once when a texture is loaded (docs/RECOMPILATION_PLAN.md §11.2, the owner's
 * decision 2 of §11.8): ETC1 handed to the GPU as ETC2 (it reads ETC1 data unchanged: the blocks only reordered and
 * byte-swapped), every other format, and ETC1A4 (ETC2 has no 4-bit alpha), decoded exactly to RGBA8.
 *
 * The decoding is Azahar's (src/video_core/texture/texture_decode.cpp, etc1.cpp; the frames are compared against
 * Azahar's): 8x8 tiles in Morton order, the bit expansions (v << 3 | v >> 2 for 5 bits, ...), ETC1 as Azahar samples it.
 * Row y of the output is Azahar's texture coordinate y (its sampling decides which way is up).
 *
 * The one ETC1 case ETC2 reads otherwise: a differential block whose base plus delta leaves 0-31 (ETC1 leaves it
 * undefined; ETC2 makes it its T, H or planar modes; Azahar decodes it with the out-of-range value). A texture with such
 * a block is not transcoded: pica_texture_etc1_to_etc2 says so and the texture is decoded to RGBA8 instead. */
#ifndef PICA_TEXTURE_H
#define PICA_TEXTURE_H

#include <stddef.h>
#include <stdint.h>

enum {
    PICA_RGBA8 = 0, PICA_RGB8 = 1, PICA_RGB5A1 = 2, PICA_RGB565 = 3, PICA_RGBA4 = 4, PICA_IA8 = 5, PICA_RG8 = 6,
    PICA_I8 = 7, PICA_A8 = 8, PICA_IA4 = 9, PICA_I4 = 10, PICA_A4 = 11, PICA_ETC1 = 12, PICA_ETC1A4 = 13
};

/* the bytes a width x height texture of that format takes (width and height multiples of 8); 0 for an unknown format */
size_t pica_texture_size(uint32_t format, uint32_t width, uint32_t height);

/* the texel at (x, y), as Azahar's LookupTexture (disable_alpha off): RGBA, one byte each */
void pica_texture_texel(uint32_t format, const uint8_t* data, uint32_t width, uint32_t x, uint32_t y, uint8_t rgba[4]);

/* the whole texture to RGBA8, width * height * 4 bytes, row y at out + y * width * 4 */
void pica_texture_decode(uint32_t format, const uint8_t* data, uint32_t width, uint32_t height, uint8_t* out);

/* an ETC1 texture as ETC2 RGB8 blocks (VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: 4x4 blocks row by row, each 8 bytes
   big-endian), width * height / 2 bytes; 0 when done, -1 when a block would read otherwise as ETC2 (then decode). */
int pica_texture_etc1_to_etc2(const uint8_t* data, uint32_t width, uint32_t height, uint8_t* out);

#endif
