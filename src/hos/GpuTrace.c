/* The GPU trace: see GpuTrace.h. */
#ifdef HOS_GPU_TRACE
#include "GpuTrace.h"

#include <stdio.h>
#include <stdlib.h>

#include "Memory.h"

__thread uint32_t hos_fn_stack[8192];
__thread int hos_fn_depth;

enum { SPAN = 0x08000000u, TABLE = 1 << 16 }; /* the linear heap's span; the writers' table (open addressing) */

static uint32_t* writer; /* a word's writer, by its offset in the linear heap */
static struct { uint32_t fn, caller, words, lists; } table[TABLE];
static uint32_t last_list[TABLE]; /* the list number a writer was last counted in */
static unsigned long long lists, words, unknown, outside;

void hos_gpu_trace_store(uint32_t a)
{
    if (a - HOS_LINEAR >= SPAN || hos_fn_depth <= 0 || hos_fn_depth > 8192)
        return;
    if (!writer && !(writer = calloc(SPAN / 4, 4)))
        return;
    writer[(a - HOS_LINEAR) / 4] = hos_fn_stack[hos_fn_depth - 1];
}

static int slot(uint32_t fn)
{
    uint32_t h = (fn * 2654435761u) >> 16;
    while (table[h].fn && table[h].fn != fn)
        h = (h + 1) % TABLE;
    return (int)h;
}

void hos_gpu_trace_list(uint32_t address, uint32_t size)
{
    lists++;
    if (address - HOS_LINEAR >= SPAN || !writer) {
        outside++;
        return;
    }
    for (uint32_t o = 0; o < size && address + o - HOS_LINEAR < SPAN; o += 4) {
        uint32_t fn = writer[(address + o - HOS_LINEAR) / 4];
        words++;
        if (!fn) {
            unknown++;
            continue;
        }
        int h = slot(fn);
        table[h].fn = fn;
        table[h].words++;
        if (last_list[h] != lists) {
            last_list[h] = (uint32_t)lists;
            table[h].lists++;
        }
    }
}

void hos_gpu_trace_write(void)
{
    const char* path = getenv("HOS_GPU_TRACE");
    FILE* f = path ? fopen(path, "w") : NULL;
    if (!f)
        return;
    fprintf(f, "# lists %llu, words %llu, words no translated function wrote %llu, lists outside the linear heap %llu\n",
            lists, words, unknown, outside);
    fprintf(f, "# writer\twords\tlists\n");
    for (int i = 0; i < TABLE; i++)
        if (table[i].fn)
            fprintf(f, "%08X\t%u\t%u\n", table[i].fn, table[i].words, table[i].lists);
    fclose(f);
}
#endif
