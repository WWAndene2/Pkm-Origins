/* The GPU trace: see GpuTrace.h.
 *
 * Every word of the heap (0x08000000), the linear heap (0x14000000) and the program's region (0x00100000-0x04100000: its
 * data and bss, the modules) has an origin, kept in a shadow table: the
 * function chain that stored it (a writer: the function and its 3 callers), or the place in the game's RomFS a file
 * read put it (fs:USER), or nothing known. A copy function (memcpy: $HOS_GPU_TRACE_COPY, addresses in hex separated by
 * commas, by default Omega Ruby's 00144D28) passes its sources' origins on: its loads are queued and each of its
 * stores takes the oldest one's origin (ldm then stm, ldrb then strb: the same order), so a command list copied from
 * a buffer read from a file is credited to the file, not to memcpy. A copy from a word of no known origin (the
 * program's own tables, as loaded) is credited to the copy's caller chain, marked as such, with the addresses.
 */
#ifdef HOS_GPU_TRACE
#include "GpuTrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Memory.h"

__thread uint32_t hos_fn_stack[8192];
__thread int hos_fn_depth;

enum { SPAN = 0x08000000u, TABLE = 1 << 16, READS = 1 << 20, FRAMES = 4 };
enum { FROM_READ = 0x80000000u, FROM_COPY = 0x40000000u }; /* (an origin: a chain's index + 1, or one of these) */

static uint32_t* shadow[3]; /* the heap's, the linear heap's and the program's (its data, bss, modules) words' origins */
static struct {
    uint32_t fn[FRAMES];
    unsigned long long words, lists, copied; /* copied: of the words, those a copy took from outside the heaps */
    uint32_t last, from_lo, from_hi;         /* (and the addresses it took them from) */
} table[TABLE];
static struct { uint64_t offset; uint32_t address, length; unsigned long long words; } reads[READS];
static uint32_t read_count;
static unsigned long long lists, words, unknown, outside;
static uint32_t copies[16];
static int copy_count = -1;
static __thread uint32_t queue[64];
static __thread unsigned head, tail;

static uint32_t* origin_of(uint32_t a)
{
    static const uint32_t base[3] = {0x08000000u, HOS_LINEAR, 0x00100000u};
    int k = a - base[0] < SPAN ? 0 : a - base[1] < SPAN ? 1 : a - base[2] < 0x04000000u ? 2 : -1;
    if (k < 0)
        return NULL;
    if (!shadow[k] && !(shadow[k] = calloc((k == 2 ? 0x04000000u : SPAN) / 4, 4)))
        return NULL;
    return &shadow[k][(a - base[k]) / 4];
}

static int in_copy(void)
{
    if (copy_count < 0) {
        const char* s = getenv("HOS_GPU_TRACE_COPY");
        copy_count = 0;
        for (s = s ? s : "144D28"; *s && copy_count < 16;) {
            char* e;
            copies[copy_count++] = (uint32_t)strtoul(s, &e, 16);
            s = *e == ',' ? e + 1 : e + strlen(e);
        }
    }
    if (hos_fn_depth <= 0 || hos_fn_depth > 8192)
        return 0;
    for (int i = 0; i < copy_count; i++)
        if (hos_fn_stack[hos_fn_depth - 1] == copies[i])
            return 1;
    return 0;
}

static uint32_t chain_here(int skip)
{
    uint32_t chain[FRAMES] = {0}, h = 0;
    for (int i = 0; i < FRAMES && skip + i < hos_fn_depth; i++) {
        chain[i] = hos_fn_stack[hos_fn_depth - 1 - skip - i];
        h = (h ^ chain[i]) * 2654435761u;
    }
    h >>= 16;
    for (int n = 0; table[h].fn[0] && memcmp(table[h].fn, chain, sizeof chain); n++) {
        if (n == TABLE)
            return 0; /* (full) */
        h = (h + 1) % TABLE;
    }
    memcpy(table[h].fn, chain, sizeof chain);
    return h + 1;
}

void hos_gpu_trace_load(uint32_t a)
{
    if (in_copy() && head - tail < 64)
        queue[head++ % 64] = a;
}

void hos_gpu_trace_store(uint32_t a)
{
    if (hos_fn_depth <= 0 || hos_fn_depth > 8192)
        return;
    uint32_t* o = origin_of(a);
    if (in_copy()) {
        uint32_t from = head != tail ? queue[tail++ % 64] : 0;
        uint32_t* s = from ? origin_of(from) : NULL;
        if (o && s && *s)
            *o = *s;
        else if (o) { /* (a source outside the heaps: the caller of the copy, and where it copied from) */
            uint32_t h = chain_here(1);
            *o = FROM_COPY | h;
            if (h && from) {
                if (!table[h - 1].from_lo || from < table[h - 1].from_lo)
                    table[h - 1].from_lo = from;
                if (from > table[h - 1].from_hi)
                    table[h - 1].from_hi = from;
            }
        }
        return;
    }
    head = tail = 0;
    if (o)
        *o = chain_here(0);
}

void hos_gpu_trace_file(uint32_t address, uint32_t length, uint64_t offset)
{
    if (read_count >= READS)
        return;
    uint32_t r = read_count++;
    reads[r].offset = offset;
    reads[r].address = address;
    reads[r].length = length;
    for (uint32_t k = 0; k < length; k += 4) {
        uint32_t* o = origin_of(address + k);
        if (o)
            *o = FROM_READ | r;
    }
}

void hos_gpu_trace_list(uint32_t address, uint32_t size)
{
    lists++;
    if (address - HOS_LINEAR >= SPAN) {
        outside++;
        return;
    }
    for (uint32_t k = 0; k < size; k += 4) {
        uint32_t* o = origin_of(address + k);
        uint32_t id = o ? *o : 0;
        words++;
        if (!id) {
            unknown++;
        } else if (id & FROM_READ) {
            reads[id & ~FROM_READ].words++;
        } else {
            uint32_t h = (id & ~FROM_COPY) - 1;
            table[h].words += 1;
            if (id & FROM_COPY)
                table[h].copied++;
            if (table[h].last != lists) {
                table[h].last = (uint32_t)lists;
                table[h].lists++;
            }
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
    fprintf(f, "# lists %llu, words %llu, of no known origin %llu, lists outside the linear heap %llu\n", lists, words,
            unknown, outside);
    fprintf(f, "# kind\twriter, or copy's caller (or RomFS offset)\tits caller (or length)\tthat one's\tand its\twords"
               "\tlists\tcopied from\n");
    for (int i = 0; i < TABLE; i++) {
        if (table[i].words > table[i].copied)
            fprintf(f, "write\t%08X\t%08X\t%08X\t%08X\t%llu\t%llu\t-\n", table[i].fn[0], table[i].fn[1],
                    table[i].fn[2], table[i].fn[3], table[i].words - table[i].copied, table[i].lists);
        if (table[i].copied)
            fprintf(f, "copy\t%08X\t%08X\t%08X\t%08X\t%llu\t%llu\t%08X-%08X\n", table[i].fn[0], table[i].fn[1],
                    table[i].fn[2], table[i].fn[3], table[i].copied, table[i].lists, table[i].from_lo,
                    table[i].from_hi);
    }
    for (uint32_t r = 0; r < read_count; r++)
        if (reads[r].words)
            fprintf(f, "romfs\t%010llX\t%X\t-\t-\t%llu\t-\t-\n", (unsigned long long)reads[r].offset, reads[r].length,
                    reads[r].words);
    fclose(f);
}
#endif
