/* The game's memory: its whole 32-bit address space as one host reservation (hos_memory, Cpu.h), the regions the 3DS
 * maps for an application made accessible in it as the kernel maps them (Memory.c).
 *
 * The 3DS layout (Azahar's core/memory.h, as the kernel gives it to an application):
 *   0x00100000  the program: text, read-only data, data and bss (the exheader's sizes); the CRO modules are mapped
 *               after it, at addresses the game chooses (ldr:ro LoadCRO), up to 0x04000000 (IPC mappings from there)
 *   0x08000000  the heap (svc ControlMemory, the game's allocator)
 *   0x10000000  shared memory blocks (MapMemoryBlock); the main thread's stack ends here
 *   0x14000000  the linear heap (ControlMemory with the linear flag: GPU buffers)
 *   0x1EC00000  I/O registers, 0x1F000000 VRAM, 0x1FF00000 the DSP's memory, 0x1FF80000 the kernel's configuration
 *   page, 0x1FF81000 the shared page, 0x1FF82000 the threads' local storage (0x200 each)
 */
#ifndef HOS_MEMORY_H
#define HOS_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/* reserves the 4 GiB (nothing committed); 0 on success */
int hos_memory_init(void);

/* makes [address, address + size) usable (page-aligned, zeroed), readable and writable; 0 on success */
int hos_memory_map(uint32_t address, uint32_t size);

/* makes [address, address + size) unusable again */
void hos_memory_unmap(uint32_t address, uint32_t size);

/* the host pointer of a game address */
static inline void* hos_pointer(uint32_t address)
{
    extern uint8_t* hos_memory;
    return hos_memory + address;
}

enum {
    HOS_PROGRAM = 0x00100000u,
    HOS_PROGRAM_END = 0x04000000u,
    HOS_HEAP = 0x08000000u,
    HOS_SHARED_MEMORY = 0x10000000u,
    HOS_STACK_END = 0x10000000u,
    HOS_LINEAR = 0x14000000u,
    HOS_IO = 0x1EC00000u,
    HOS_VRAM = 0x1F000000u,
    HOS_DSP = 0x1FF00000u,
    HOS_CONFIG = 0x1FF80000u,
    HOS_SHARED_PAGE = 0x1FF81000u,
    HOS_TLS = 0x1FF82000u,
    HOS_TLS_SIZE = 0x200u,
};

#endif
