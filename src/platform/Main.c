/* The recompiled game's entry: the player's extracted game (tools/n3ds_extract.py: code.bin, exheader.bin, the CRO
 * modules; the RomFS read from the player's .3ds) loaded into the game's memory, the system started, the game's main
 * thread run from the program's entry, as the 3DS kernel starts an application.
 *
 *   game <extracted dir> <the game's .3ds>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hos/Kernel.h"
#include "../hos/Loader.h"
#include "../hos/Memory.h"

static uint8_t* slurp(const char* dir, const char* name, size_t* n)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* p = malloc(*n);
    if (!p || fread(p, 1, *n, f) != *n) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    return p;
}

const char* hos_game_dir;
const char* hos_game_image;

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <extracted dir> <the game's .3ds>\n", argv[0]);
        return 2;
    }
    hos_game_dir = argv[1];
    hos_game_image = argv[2];
    size_t code_size, exheader_size;
    uint8_t* code = slurp(argv[1], "code.bin", &code_size);
    uint8_t* exheader = slurp(argv[1], "exheader.bin", &exheader_size);
    if (!code || !exheader || exheader_size < 0x400)
        return 1;
    if (hos_memory_init()) {
        fprintf(stderr, "hos: could not reserve the game's address space\n");
        return 1;
    }
    HosProgram program;
    if (hos_load_program(code, code_size, exheader, &program))
        return 1;
    free(code);
    hos_clock_start();
    HosThread* main_thread = hos_thread_create(program.entry, 0, program.stack_top, program.priority, program.tls);
    fprintf(stderr, "hos: starting the game at %08X (priority %d)\n", program.entry, program.priority);
    hos_thread_start_main(main_thread);
    fprintf(stderr, "hos: the main thread ended\n");
    return 0;
}
