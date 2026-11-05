/* The program loaded as the 3DS kernel loads an application (Azahar's loader/ncch.cpp and kernel/process.cpp,
 * Process::Run): its code.bin at the exheader's addresses, bss zeroed after the data, the main thread's stack ending at
 * 0x10000000, its local storage at 0x1FF82000, the kernel's configuration page and the shared page filled as Azahar
 * fills them (kernel/config_mem.cpp, kernel/shared_page.cpp), and ControlMemory for the heaps.
 */
#include "Loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

#include "Kernel.h"
#include "Memory.h"

static uint32_t committed;
static uint32_t linear_next = HOS_LINEAR; /* the linear heap's next free address (a bump: what the games free is rare) */

uint32_t hos_memory_committed(void)
{
    return committed;
}

static uint32_t u32_at(const uint8_t* p, size_t off)
{
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

/* the memory modes' application region sizes (Azahar's kernel/memory.cpp memory_region_sizes, column 0) */
static const uint32_t app_memory[6] = {0x04000000, 0x04000000, 0x06000000, 0x05000000, 0x04800000, 0x02000000};

int hos_load_program(const uint8_t* code, size_t code_size, const uint8_t* exheader, HosProgram* out)
{
    uint32_t text = u32_at(exheader, 0x10), text_pages = u32_at(exheader, 0x14);
    uint32_t ro = u32_at(exheader, 0x20), ro_pages = u32_at(exheader, 0x24);
    uint32_t data = u32_at(exheader, 0x30), data_size = u32_at(exheader, 0x38);
    uint32_t stack = u32_at(exheader, 0x1C), bss = u32_at(exheader, 0x3C);
    /* (the access control info's ARM11 local capabilities, at 0x200: flag0 at + 0xE, the main thread's priority + 0xF) */
    uint8_t flag0 = exheader[0x20E], priority = exheader[0x20F];
    (void)ro;
    if (text != HOS_PROGRAM || (size_t)(text_pages + ro_pages) * 0x1000 + data_size > code_size + 0x1000) {
        fprintf(stderr, "hos: the exheader does not describe this code.bin\n");
        return -1;
    }
    uint32_t end = (data + data_size + bss + 0xFFFu) & ~0xFFFu;
    if (hos_memory_map(HOS_PROGRAM, end - HOS_PROGRAM))
        return -1;
    memcpy(hos_pointer(HOS_PROGRAM), code, code_size);
    memset(hos_pointer(data + data_size), 0, bss); /* (bss: zero, past the data) */
    committed += end - HOS_PROGRAM;
    /* the main thread's stack, in the heap's top (Process::Run: HEAP_VADDR_END - stack size) */
    uint32_t stack_size = (stack + 0xFFFu) & ~0xFFFu;
    if (hos_memory_map(HOS_STACK_END - stack_size, stack_size))
        return -1;
    committed += stack_size;
    /* the threads' local storage, the configuration page and the shared page */
    if (hos_memory_map(HOS_CONFIG, 0x2000) || hos_memory_map(HOS_TLS, 0x7E000))
        return -1;
    /* the VRAM (6 MB, Azahar's VRAM_VADDR): the GPU's DMAs and memory fills write there */
    if (hos_memory_map(0x1F000000u, 0x600000))
        return -1;
    uint8_t* cfg = hos_pointer(HOS_CONFIG);
    unsigned mode = (flag0 >> 4) & 0xF;
    cfg[2] = 0x3A; /* kernel version: 2.58 (config_mem.cpp) */
    cfg[3] = 0x02;
    uint64_t ns_tid = 0x0004013000008002ull;
    memcpy(cfg + 0x08, &ns_tid, 8);
    uint32_t v = 2;
    memcpy(cfg + 0x10, &v, 4); /* sys_core_ver */
    cfg[0x14] = 1;             /* unit_info: retail */
    cfg[0x16] = 1;             /* prev_firm */
    v = 0x0000F450;
    memcpy(cfg + 0x18, &v, 4); /* ctr_sdk_ver */
    v = mode;
    memcpy(cfg + 0x30, &v, 4); /* app_mem_type */
    v = app_memory[mode < 6 ? mode : 0];
    memcpy(cfg + 0x40, &v, 4); /* app_mem_alloc */
    static const uint32_t sys_memory[6] = {0x02C00000, 0x02C00000, 0x00C00000, 0x01C00000, 0x02400000, 0x04C00000};
    v = sys_memory[mode < 6 ? mode : 0];
    memcpy(cfg + 0x44, &v, 4);
    v = 0x01400000;
    memcpy(cfg + 0x48, &v, 4);
    cfg[0x62] = 0x3A;
    cfg[0x63] = 0x02;
    v = 2;
    memcpy(cfg + 0x64, &v, 4);
    v = 0x0000F450;
    memcpy(cfg + 0x68, &v, 4);
    uint8_t* page = hos_pointer(HOS_SHARED_PAGE);
    page[0x04] = 1;    /* running_hw: a retail unit */
    page[0x85] = 5 << 2 | 2 | 1; /* battery: completely full (ptm.h's level 5), charging, adapter in */
    page[0x66] = 3;              /* wifi link level: best, and */
    page[0x67] = 2;              /* state: internet (as Azahar sets them, for NDM) */
    page[0x86] = 1;    /* (Azahar: some games wait for this to be 1) */
    float slider = 0;
    memcpy(page + 0x80, &slider, 4);
    hos_update_time();
    out->entry = text;
    out->stack_top = HOS_STACK_END;
    out->priority = priority;
    out->tls = HOS_TLS;
    return 0;
}

/* the shared page's clock: milliseconds since 1900 and the tick it was read at (shared_page.cpp's date_time) */
void hos_update_time(void)
{
    uint8_t* page = hos_pointer(HOS_SHARED_PAGE);
    uint32_t counter;
    memcpy(&counter, page, 4);
    uint8_t* dt = page + (counter % 2 ? 0x20 : 0x40); /* the one not being read */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    /* local time, as Azahar's system clock setting gives the 3DS (1900 to 1970: 2,208,988,800 s) */
    time_t now = ts.tv_sec;
    struct tm local;
    localtime_r(&now, &local);
    int64_t ms = ((int64_t)now + local.tm_gmtoff + 2208988800LL) * 1000 + ts.tv_nsec / 1000000;
    uint64_t tick = hos_ticks(), rate = 268111856;
    memcpy(dt, &ms, 8);
    memcpy(dt + 8, &tick, 8);
    memcpy(dt + 0x10, &rate, 8);
    memset(dt + 0x18, 0, 8); /* tick_offset */
    counter++;
    memcpy(page, &counter, 4);
}

/* svc ControlMemory (Azahar's svc.cpp / process.cpp): operation 3 commits (maps) memory, the linear flag (0x10000)
   in the linear heap at an address the kernel chooses, 1 frees it, 4 shows memory at a second address, 5 undoes that,
   6 changes its protection */
HosResult hos_control_memory(uint32_t* out, uint32_t op, uint32_t addr0, uint32_t addr1, uint32_t size, uint32_t perm)
{
    (void)addr1;
    uint32_t kind = op & 0xFF;
    int linear = (op & 0x10000) != 0;
    size = (size + 0xFFFu) & ~0xFFFu;
    if (kind == 3) { /* commit */
        uint32_t at = addr0;
        if (linear) {
            if (!at) {
                at = linear_next;
                linear_next += size;
            }
        } else if (at < HOS_HEAP || at + size > HOS_STACK_END) {
            return HOS_ERR_INVALID_ADDRESS;
        }
        if (hos_memory_map(at, size))
            return HOS_ERR_OUT_OF_MEMORY;
        committed += size;
        *out = at;
        return HOS_OK;
    }
    if (kind == 1) { /* free */
        hos_memory_unmap(addr0, size);
        committed -= size;
        *out = addr0;
        return HOS_OK;
    }
    if (kind == 4) { /* map: addr0 shows addr1's bytes (Azahar's Process::Map) */
        if (addr1 < HOS_HEAP || addr1 + size > HOS_STACK_END || addr1 - addr0 < size || addr0 - addr1 < size)
            return HOS_ERR_INVALID_ADDRESS;
        if (hos_memory_alias(addr0, addr1, size))
            return HOS_ERR_OUT_OF_MEMORY;
        *out = 0;
        return HOS_OK;
    }
    if (kind == 5) { /* unmap such a view */
        hos_memory_unalias(addr0, size);
        *out = 0;
        return HOS_OK;
    }
    if (kind == 6) { /* protect (Azahar: ReprotectRange): none, or readable (and writable: the host's translated code
                        runs from no page, so execute changes nothing) */
        uint32_t start = addr0 & ~0xFFFu, end = (addr0 + size + 0xFFFu) & ~0xFFFu;
        if (mprotect(hos_memory + start, end - start, (perm & 3) ? PROT_READ | PROT_WRITE : PROT_NONE))
            return HOS_ERR_INVALID_ADDRESS;
        *out = 0;
        return HOS_OK;
    }
    fprintf(stderr, "hos: ControlMemory operation %X not done\n", op);
    return HOS_ERR_NOT_IMPLEMENTED;
}
