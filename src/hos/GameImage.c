/* The player's .3ds (GameImage.h): NCSD header (0x100 "NCSD", partitions at 0x120 in 0x200-byte units), the first
 * partition's NCCH (ExeFS at + 0x1A0, RomFS at + 0x1B0, in units), the ExeFS's table of 10 files (name, offset, size;
 * data from + 0x200). */
#include "GameImage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* image;
static uint64_t ncch, exefs, romfs, romfs_bytes;

static uint32_t u32_of(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

int hos_game_open(const char* path)
{
    image = fopen(path, "rb");
    if (!image)
        return -1;
    uint8_t h[0x200];
    if (fread(h, 1, sizeof h, image) != sizeof h || memcmp(h + 0x100, "NCSD", 4))
        return -1;
    ncch = (uint64_t)u32_of(h + 0x120) * 0x200;
    if (fseeko(image, (off_t)ncch, SEEK_SET) || fread(h, 1, sizeof h, image) != sizeof h || memcmp(h + 0x100, "NCCH", 4))
        return -1;
    exefs = ncch + (uint64_t)u32_of(h + 0x1A0) * 0x200;
    romfs = ncch + (uint64_t)u32_of(h + 0x1B0) * 0x200 + 0x1000;
    romfs_bytes = (uint64_t)u32_of(h + 0x1B4) * 0x200 - 0x1000;
    return 0;
}

size_t hos_romfs_read(uint64_t offset, void* out, size_t size)
{
    if (offset >= romfs_bytes)
        return 0;
    if (size > romfs_bytes - offset)
        size = (size_t)(romfs_bytes - offset);
    if (fseeko(image, (off_t)(romfs + offset), SEEK_SET))
        return 0;
    return fread(out, 1, size, image);
}

uint64_t hos_romfs_size(void)
{
    return romfs_bytes;
}

uint8_t* hos_exefs_file(const char* name, uint32_t* size)
{
    uint8_t h[0x200];
    if (fseeko(image, (off_t)exefs, SEEK_SET) || fread(h, 1, sizeof h, image) != sizeof h)
        return NULL;
    for (int i = 0; i < 10; i++) {
        const uint8_t* e = h + 16 * i;
        if (strncmp((const char*)e, name, 8))
            continue;
        *size = u32_of(e + 12);
        uint8_t* p = malloc(*size ? *size : 1);
        if (fseeko(image, (off_t)(exefs + 0x200 + u32_of(e + 8)), SEEK_SET) || fread(p, 1, *size, image) != *size) {
            free(p);
            return NULL;
        }
        return p;
    }
    return NULL;
}

int hos_game_regions(uint32_t* out)
{
    uint32_t size;
    uint8_t* smdh = hos_exefs_file("icon", &size);
    int n = 0;
    if (smdh && size >= 0x201C) { /* (the SMDH's region lockout at 0x2018, as Azahar's ParseRegionLockoutInfo) */
        uint32_t lockout = u32_of(smdh + 0x2018);
        for (uint32_t r = 0; r < 7; r++)
            if (lockout & (1u << r))
                out[n++] = r;
    }
    free(smdh);
    return n;
}
