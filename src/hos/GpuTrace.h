/* The GPU trace (built with -DHOS_GPU_TRACE: tools/build_game.py --gpu-trace): which of the game's functions wrote
 * the words of the GPU command lists it submits, to find its graphics library (docs/RECOMPILATION_PLAN.md §3, step 4).
 * Every store into the linear heap records its writer, the translated function it ran in (tools/arm2c.py's
 * HOS_FN_IN); each submitted list then credits its words to their writers. The table is written to the file
 * $HOS_GPU_TRACE every time the GSP reports (Gsp.c) and holds addresses and counts only. Nothing in other builds.
 */
#ifndef HOS_GPU_TRACE_H
#define HOS_GPU_TRACE_H

#include <stdint.h>

#ifdef HOS_GPU_TRACE
void hos_gpu_trace_list(uint32_t address, uint32_t size);
void hos_gpu_trace_write(void);
#else
#define hos_gpu_trace_list(a, s) ((void)0)
#define hos_gpu_trace_write() ((void)0)
#endif

#endif
