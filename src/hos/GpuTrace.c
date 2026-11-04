/* The GPU trace: see GpuTrace.h. */
#ifdef HOS_GPU_TRACE
#include "GpuTrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Memory.h"

__thread uint32_t hos_fn_stack[8192];
__thread int hos_fn_depth;

enum { SPAN = 0x08000000u, TABLE = 1 << 16 }; /* the linear heap's span; the writers' table (open addressing) */

enum { FRAMES = 4 }; /* a writer is recorded with its callers: the function and the 3 it was called from */
static uint32_t* writer; /* a word's writer (an index in table, + 1), by its offset in the linear heap */
static struct { uint32_t fn[FRAMES], words, lists; } table[TABLE];
static uint32_t last_list[TABLE]; /* the list number a writer was last counted in */
static unsigned long long lists, words, unknown, outside;

void hos_gpu_trace_store(uint32_t a)
{
    if (a - HOS_LINEAR >= SPAN || hos_fn_depth <= 0 || hos_fn_depth > 8192)
        return;
    if (!writer && !(writer = calloc(SPAN / 4, 4)))
        return;
    uint32_t chain[FRAMES] = {0}, h = 0;
    for (int i = 0; i < FRAMES && i < hos_fn_depth; i++) {
        chain[i] = hos_fn_stack[hos_fn_depth - 1 - i];
        h = (h ^ chain[i]) * 2654435761u;
    }
    h >>= 16;
    for (int n = 0; table[h].fn[0] && memcmp(table[h].fn, chain, sizeof chain); n++) {
        if (n == TABLE)
            return; /* (full) */
        h = (h + 1) % TABLE;
    }
    memcpy(table[h].fn, chain, sizeof chain);
    writer[(a - HOS_LINEAR) / 4] = h + 1;
}

void hos_gpu_trace_list(uint32_t address, uint32_t size)
{
    lists++;
    if (address - HOS_LINEAR >= SPAN || !writer) {
        outside++;
        return;
    }
    for (uint32_t o = 0; o < size && address + o - HOS_LINEAR < SPAN; o += 4) {
        uint32_t id = writer[(address + o - HOS_LINEAR) / 4];
        words++;
        if (!id) {
            unknown++;
            continue;
        }
        int h = (int)id - 1;
        table[h].words++;
        if (last_list[h] != lists) {
            last_list[h] = (uint32_t)lists;
            table[h].lists++;
        }
    }
    if (lists <= 16 || !(lists % 256))
        hos_gpu_trace_write(); /* (also early: a run can stop before the GSP's first report) */
}

void hos_gpu_trace_write(void)
{
    const char* path = getenv("HOS_GPU_TRACE");
    FILE* f = path ? fopen(path, "w") : NULL;
    if (!f)
        return;
    fprintf(f, "# lists %llu, words %llu, words no translated function wrote %llu, lists outside the linear heap %llu\n",
            lists, words, unknown, outside);
    fprintf(f, "# writer\tits caller\tthat one's\tand its\twords\tlists (only writers with words in a list)\n");
    for (int i = 0; i < TABLE; i++)
        if (table[i].words)
            fprintf(f, "%08X\t%08X\t%08X\t%08X\t%u\t%u\n", table[i].fn[0], table[i].fn[1], table[i].fn[2],
                    table[i].fn[3], table[i].words, table[i].lists);
    fclose(f);
}
#endif
