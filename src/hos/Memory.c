/* The game's address space as one host reservation (Memory.h). POSIX (Linux, Android) for now.
 *
 * The reservation is a view of a 4 GB memory file (memfd, sparse: pages exist once written), each address at its own
 * offset, so that a page can also be seen at a second address (svc ControlMemory MAP: the same bytes, as the 3DS's
 * kernel maps them; Azahar's Process::Map): that address is remapped onto the source's offset. */
#include "Memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

uint8_t* hos_memory;
static int file = -1;

int hos_memory_init(void)
{
    file = (int)syscall(SYS_memfd_create, "hos_memory", 0); /* (the call itself: Android's libc declares
                                                                memfd_create only from API 30, the kernel has it) */
    if (file < 0 || ftruncate(file, (off_t)1 << 32))
        return -1;
    void* p = mmap(NULL, (size_t)1 << 32, PROT_NONE, MAP_SHARED | MAP_NORESERVE, file, 0);
    if (p == MAP_FAILED)
        return -1;
    hos_memory = p;
    return 0;
}

int hos_memory_alias(uint32_t target, uint32_t source, uint32_t size)
{
    size = (size + 0xFFFu) & ~0xFFFu;
    void* p = mmap(hos_memory + target, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, file, (off_t)source);
    return p == MAP_FAILED ? -1 : 0;
}

void hos_memory_unalias(uint32_t target, uint32_t size)
{
    size = (size + 0xFFFu) & ~0xFFFu; /* (back to its own offset, unusable) */
    mmap(hos_memory + target, size, PROT_NONE, MAP_SHARED | MAP_FIXED, file, (off_t)target);
}

int hos_memory_map(uint32_t address, uint32_t size)
{
    uint32_t start = address & ~0xFFFu, end = (address + size + 0xFFFu) & ~0xFFFu;
    return mprotect(hos_memory + start, (size_t)(end - start), PROT_READ | PROT_WRITE);
}

void hos_memory_unmap(uint32_t address, uint32_t size)
{
    uint32_t start = address & ~0xFFFu, end = (address + size + 0xFFFu) & ~0xFFFu;
    fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, start, end - start); /* (its pages freed: zero again) */
    mprotect(hos_memory + start, (size_t)(end - start), PROT_NONE);
}
