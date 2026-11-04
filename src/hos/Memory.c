/* The game's address space as one host reservation (Memory.h). POSIX (Linux, Android) for now. */
#include "Memory.h"

#include <sys/mman.h>

uint8_t* hos_memory;

int hos_memory_init(void)
{
    void* p = mmap(NULL, (size_t)1 << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    hos_memory = p;
    return 0;
}

int hos_memory_map(uint32_t address, uint32_t size)
{
    uint32_t start = address & ~0xFFFu, end = (address + size + 0xFFFu) & ~0xFFFu;
    return mprotect(hos_memory + start, (size_t)(end - start), PROT_READ | PROT_WRITE);
}

void hos_memory_unmap(uint32_t address, uint32_t size)
{
    uint32_t start = address & ~0xFFFu, end = (address + size + 0xFFFu) & ~0xFFFu;
    madvise(hos_memory + start, (size_t)(end - start), MADV_DONTNEED);
    mprotect(hos_memory + start, (size_t)(end - start), PROT_NONE);
}
