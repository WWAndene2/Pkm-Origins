/* The state the translated game code runs on (tools/arm2c.py writes C against it), and its memory.
 *
 * Cpu: one ARM11 thread's registers as the translated C keeps them: r0-r15, the flags N Z C V one per word, the VFP
 * registers as bits (s[32]; d[k] is s[2k] low, s[2k+1] high) and FPSCR, the thread's local storage pointer (what
 * mrc p15, 0, rX, c13, c0, 3 reads), the exclusive monitor (ldrex / strex), and two counters the translated functions
 * keep (steps: loop iterations, depth: call depth) for the matching checks. fault is 0 while all is well; the
 * translated code returns at once when it is set (src/hos/ sets it when a thread must stop).
 *
 * Memory: the game's whole 32-bit address space is one reservation (Memory.c), the game's address a an offset into
 * it, so a load is a host load. The 3DS allows unaligned word loads and stores (ARMv6 with alignment checking off, as
 * the kernel sets it): memcpy does them on any host.
 */
#ifndef HOS_CPU_H
#define HOS_CPU_H

#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct {
    uint32_t r[16];
    uint32_t n, z, c, v;
    int fault;
    uint32_t steps;
    uint32_t s[32], fpscr;
    uint32_t tls, mon, mon_addr, depth;
} Cpu;

extern uint8_t* hos_memory;

static inline uint32_t ld32(Cpu* c, uint32_t a) { uint32_t v; (void)c; memcpy(&v, hos_memory + a, 4); return v; }
static inline uint32_t ld16(Cpu* c, uint32_t a) { uint16_t v; (void)c; memcpy(&v, hos_memory + a, 2); return v; }
static inline uint32_t ld8(Cpu* c, uint32_t a) { (void)c; return hos_memory[a]; }
#ifdef HOS_GPU_TRACE
void hos_gpu_trace_store(uint32_t a); /* (GpuTrace.c: the store's writer recorded, in the linear heap) */
#define HOS_STORE_SEEN(a) hos_gpu_trace_store(a)
#else
#define HOS_STORE_SEEN(a) ((void)0)
#endif
static inline void st32(Cpu* c, uint32_t a, uint32_t v) { (void)c; HOS_STORE_SEEN(a); memcpy(hos_memory + a, &v, 4); }
static inline void st16(Cpu* c, uint32_t a, uint32_t v) { uint16_t h = (uint16_t)v; (void)c; HOS_STORE_SEEN(a); memcpy(hos_memory + a, &h, 2); }
static inline void st8(Cpu* c, uint32_t a, uint32_t v) { (void)c; HOS_STORE_SEEN(a); hos_memory[a] = (uint8_t)v; }

/* faults the translated code sets (besides the matching's own): a call that does not return returned (8), a function
   the build could not translate was called (9), an instruction the translator has no C for was reached (10); r15
   holds the address */
enum { HOS_FAULT_NO_RETURN = 8, HOS_FAULT_NOT_TRANSLATED = 9, HOS_FAULT_UNSUPPORTED = 10 };

#endif
