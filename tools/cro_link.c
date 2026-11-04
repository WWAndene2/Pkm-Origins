/* Every CRO module of a game loaded and linked at once by src/hos/Cro.c (checked against Azahar's loader by
 * tools/cro_check.c), for the matching check of the modules (tools/match_functions.py --modules all --linked): the
 * modules one after the other from 0x40000000 (where the 3DS maps nothing: the check's own place), their data and bss
 * buffers from 0x48000000, the static module where given, auto-linked, fix level 0 (nothing cut), in the order given.
 *
 *   cc -O1 -I src/hos -o cro_link tools/cro_link.c src/hos/Cro.c src/hos/Memory.c
 *   cro_link <code.bin> <static.crs> <crs address> <out dir> <module.cro>...
 *
 * Writes (outside the repository: the game's bytes) <out dir>/modules.bin (from 0x40000000), data.bin (from
 * 0x48000000), program.bin (the program's image from 0x00100000, its words the static relocations write included) and
 * modules.tsv (module, address, size).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Cro.h"
#include "Memory.h"

enum { MODULES = 0x40000000u, DATA = 0x48000000u };

static unsigned char* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* p = malloc(*n);
    if (fread(p, 1, *n, f) != *n)
        exit(2);
    fclose(f);
    return p;
}

static void spill(const char* dir, const char* name, uint32_t from, uint32_t to)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f || fwrite(hos_pointer(from), 1, to - from, f) != to - from) {
        perror(path);
        exit(2);
    }
    fclose(f);
}

static uint32_t u32_at(const unsigned char* p, size_t off)
{
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

int main(int argc, char** argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: see the comment at the top of tools/cro_link.c\n");
        return 2;
    }
    size_t code_size, crs_size;
    unsigned char* code = slurp(argv[1], &code_size);
    unsigned char* crs_file = slurp(argv[2], &crs_size);
    uint32_t crs = (uint32_t)strtoul(argv[3], NULL, 16);
    const char* out = argv[4];
    if (hos_memory_init() || hos_memory_map(HOS_PROGRAM, HOS_PROGRAM_END - HOS_PROGRAM) ||
        hos_memory_map(MODULES, 0x10000000u)) {
        fprintf(stderr, "no memory\n");
        return 2;
    }
    memcpy(hos_pointer(HOS_PROGRAM), code, code_size);
    memcpy(hos_pointer(crs), crs_file, crs_size);
    if (hos_cro_load_crs(crs, (uint32_t)crs_size)) {
        fprintf(stderr, "the static module did not load\n");
        return 1;
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/modules.tsv", out);
    FILE* map = fopen(path, "w");
    uint32_t at = MODULES, data = DATA;
    int failed = 0;
    for (int i = 5; i < argc; i++) {
        size_t size;
        unsigned char* file = slurp(argv[i], &size);
        memcpy(hos_pointer(at), file, size);
        /* the data and bss segments' sizes, from the segment table (header word 18 + 0x80: its offset; 19: count) */
        uint32_t table = u32_at(file, 0x80 + 4 * 18), count = u32_at(file, 0x80 + 4 * 19), data_size = 0, bss_size = 0;
        for (uint32_t k = 0; k < count; k++) {
            uint32_t s = u32_at(file, table + 12 * k + 4), type = u32_at(file, table + 12 * k + 8);
            if (type == 2)
                data_size = s;
            if (type == 3)
                bss_size = s;
        }
        uint32_t bss = (data + data_size + 0xFu) & ~0xFu;
        uint32_t fixed = hos_cro_load(at, (uint32_t)size, data, data_size, bss, bss_size, 1, 0, crs, 0);
        if (!fixed) {
            fprintf(stderr, "%s did not load\n", argv[i]);
            failed++;
        }
        /* the data the game would copy into its buffer: from the module, where the loader left it relocated */
        uint32_t data_off = u32_at(file, 0x80 + 4 * 14);
        if (data_size)
            memcpy(hos_pointer(data), hos_pointer(at + data_off), data_size);
        memset(hos_pointer(bss), 0, bss_size);
        char name[64] = {0};
        uint32_t name_off = u32_at(file, 0x80 + 4 * 16);
        strncpy(name, (const char*)file + name_off, sizeof name - 1);
        fprintf(map, "%s\t%08X\t%X\n", name, at, (unsigned)size);
        at = (at + (uint32_t)size + 0xFFFu) & ~0xFFFu;
        data = (bss + bss_size + 0xFFFu) & ~0xFFFu;
        free(file);
    }
    fclose(map);
    spill(out, "modules.bin", MODULES, at);
    spill(out, "data.bin", DATA, data);
    spill(out, "program.bin", HOS_PROGRAM, HOS_PROGRAM + (uint32_t)code_size);
    printf("%d modules linked (%d failed): %08X-%08X, data %08X-%08X\n", argc - 5, failed, MODULES, at, DATA, data);
    return failed != 0;
}
