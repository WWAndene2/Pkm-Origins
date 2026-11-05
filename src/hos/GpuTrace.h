/* The GPU trace (built with -DHOS_GPU_TRACE: tools/build_game.py --gpu-trace): which of the game's functions wrote
 * the words of the GPU command lists it submits, to find its graphics library (docs/RECOMPILATION_PLAN.md §3, step 4).
 * Every word stored in the heaps records its origin, the translated function chain that stored it (tools/arm2c.py's
 * HOS_FN_IN), a RomFS read, or the source a copy function took it from (GpuTrace.c); each submitted list then
 * credits its words to their origins. The table is written to the file
 * $HOS_GPU_TRACE every time the GSP reports (Gsp.c) and holds addresses and counts only. Nothing in other builds.
 */
#ifndef HOS_GPU_TRACE_H
#define HOS_GPU_TRACE_H

#include <stdint.h>

#ifdef HOS_GPU_TRACE
void hos_gpu_trace_list(uint32_t address, uint32_t size);
void hos_gpu_trace_write(void);
void hos_gpu_trace_file(uint32_t address, uint32_t length, uint64_t offset); /* (a RomFS read put these words) */
#else
#define hos_gpu_trace_list(a, s) ((void)0)
#define hos_gpu_trace_write() ((void)0)
#define hos_gpu_trace_file(a, l, o) ((void)0)
#endif

#endif
