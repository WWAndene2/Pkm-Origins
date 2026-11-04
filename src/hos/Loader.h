/* The program loaded into the game's memory, as the 3DS kernel loads an application (Loader.c). */
#ifndef HOS_LOADER_H
#define HOS_LOADER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t entry, stack_top, tls;
    int priority;
} HosProgram;

/* code.bin (decompressed) placed by its exheader (0x400 bytes), the stack, local storage and system pages mapped; 0
   on success */
int hos_load_program(const uint8_t* code, size_t code_size, const uint8_t* exheader, HosProgram* out);

/* the shared page's clock refreshed (the kernel does it every hour) */
void hos_update_time(void);

#endif
