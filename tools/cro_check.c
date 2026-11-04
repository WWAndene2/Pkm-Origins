/* Checks src/hos/Cro.c (the CRO module loader) against Azahar's: loads the static module and the modules into a fresh
 * memory where a running game had them, and compares the bytes with that game's memory (a dump of 0x00100000 up,
 * tools/azahar_trace_run.sh with a dump command): each module's fixed part, the static module, and the program's
 * text and read-only data (which the modules' static relocations write through the static module).
 *
 *   cc -O1 -I src/hos -o cro_check tools/cro_check.c src/hos/Cro.c src/hos/Memory.c
 *   cro_check <image.bin> <code.bin> <data address> <static.crs> <crs address>
 *             [<module.cro> <address> <data> <data size> <bss> <bss size> <auto link 0/1> <fix level> <bug fix 0/1>]...
 *
 * The modules in the order the game loaded them. Prints, for each range, how many words differ (and the first ones).
 * Writes nothing; the game's files are only read.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Cro.h"
#include "Memory.h"

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

static int compare(const char* what, const unsigned char* image, uint32_t from, uint32_t to)
{
    int diff = 0;
    for (uint32_t a = from; a + 4 <= to; a += 4) {
        uint32_t mine, game;
        memcpy(&mine, hos_pointer(a), 4);
        memcpy(&game, image + (a - HOS_PROGRAM), 4);
        if (mine != game) {
            if (diff < 5)
                printf("    %08X: ours %08X, the game's %08X\n", a, mine, game);
            diff++;
        }
    }
    printf("%s %08X-%08X: %d words differ\n", what, from, to, diff);
    return diff;
}

int main(int argc, char** argv)
{
    if (argc < 6 || (argc - 6) % 9) {
        fprintf(stderr, "usage: see the comment at the top of tools/cro_check.c\n");
        return 2;
    }
    size_t image_size, code_size, crs_size;
    unsigned char* image = slurp(argv[1], &image_size);
    unsigned char* code = slurp(argv[2], &code_size);
    uint32_t data_start = (uint32_t)strtoul(argv[3], NULL, 16);
    unsigned char* crs_file = slurp(argv[4], &crs_size);
    uint32_t crs = (uint32_t)strtoul(argv[5], NULL, 16);
    if (hos_memory_init() || hos_memory_map(HOS_PROGRAM, HOS_PROGRAM_END - HOS_PROGRAM) ||
        hos_memory_map(HOS_HEAP, 0x08000000u)) {
        fprintf(stderr, "no memory\n");
        return 2;
    }
    memcpy(hos_pointer(HOS_PROGRAM), code, code_size);
    memcpy(hos_pointer(crs), crs_file, crs_size);
    if (hos_cro_load_crs(crs, (uint32_t)crs_size)) {
        printf("the static module did not load\n");
        return 1;
    }
    int bad = 0;
    struct { uint32_t at, size; } loaded[64];
    int n = 0;
    for (int i = 6; i < argc; i += 9) {
        size_t size;
        unsigned char* file = slurp(argv[i], &size);
        uint32_t at = (uint32_t)strtoul(argv[i + 1], NULL, 16);
        memcpy(hos_pointer(at), file, size);
        uint32_t fixed = hos_cro_load(at, (uint32_t)size, (uint32_t)strtoul(argv[i + 2], NULL, 16),
                                      (uint32_t)strtoul(argv[i + 3], NULL, 16), (uint32_t)strtoul(argv[i + 4], NULL, 16),
                                      (uint32_t)strtoul(argv[i + 5], NULL, 16), atoi(argv[i + 6]),
                                      (uint32_t)atoi(argv[i + 7]), crs, atoi(argv[i + 8]));
        printf("%s at %08X: %s, fixed size %X\n", argv[i], at, fixed ? "loaded" : "FAILED", fixed);
        bad += !fixed;
        loaded[n].at = at;
        loaded[n++].size = fixed;
        free(file);
    }
    for (int i = 0; i < n; i++)
        bad += compare("module", image, loaded[i].at, loaded[i].at + loaded[i].size) != 0;
    bad += compare("static module", image, crs, crs + (uint32_t)crs_size) != 0;
    bad += compare("program text and read-only data", image, HOS_PROGRAM, data_start) != 0;
    printf(bad ? "DIFFERENT\n" : "the same bytes as the game's\n");
    return bad != 0;
}
