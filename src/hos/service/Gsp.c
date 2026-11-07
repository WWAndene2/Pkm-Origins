/* gsp::Gpu: the GPU service as Azahar's (service/gsp/gsp_gpu.cpp, video_core/gpu.cpp), with no GPU yet (the renderer
 * is step 4 of docs/RECOMPILATION_PLAN.md §3): the shared memory block (each thread's interrupt relay queue at
 * 0x40 its id, its command buffer at 0x800 + 0x200 its id), the GPU right, the hardware registers kept as written,
 * the commands the game queues taken from its buffer as GSP takes them and answered by the interrupts the GPU raises
 * when it is done (a command list: P3D; a memory fill: PSC0 / PSC1; a display transfer or texture copy: PPF; a DMA:
 * DMA, the copy made, as the game sees its result), at once rather than after the time the hardware takes; and the
 * VBlanks (PDC0 and PDC1) 60 times a second to every registered thread. Nothing is drawn: the command lists are
 * counted, the first one reported. */
#include "../Ipc.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../GpuTrace.h"
#include "../../platform/Renderer.h"
#include "../Memory.h"

enum { PSC0 = 0, PSC1 = 1, PDC0 = 2, PDC1 = 3, PPF = 4, P3D = 5, DMA = 6 };
enum { MAX_THREADS = 4, SLOTS = 0x34 };

static HosObject* memory;
static struct { HosObject* event; int registered; } threads[MAX_THREADS];
static uint32_t next_thread, rights = 0xFFFFFFFFu, first_initialization = 1;
static uint32_t registers[0x420000 / 4];
static uint64_t command_lists, frames;

static uint8_t* shared(uint32_t offset) { return hos_pointer(memory->block_address) + offset; }

/* an interrupt into a thread's relay queue (index, count, error, config; PDC counts past 0x20 queued are missed) */
static void signal_for(uint32_t id, int interrupt)
{
    if (id >= MAX_THREADS || !threads[id].registered || !threads[id].event || !memory->block_address)
        return; /* (nothing to write into before the game has mapped the block) */
    uint8_t* q = shared(0x40 * id);
    int pdc = interrupt == PDC0 || interrupt == PDC1;
    if (pdc && (q[3] & 1))
        return; /* (ignore_pdc) */
    if (pdc && q[1] >= 0x20) {
        uint32_t missed;
        memcpy(&missed, q + (interrupt == PDC0 ? 4 : 8), 4);
        missed++;
        memcpy(q + (interrupt == PDC0 ? 4 : 8), &missed, 4);
        return;
    }
    if (q[1] >= SLOTS) {
        q[2] = 1; /* queue full */
        return;
    }
    q[0xC + (q[0] + q[1]) % SLOTS] = (uint8_t)interrupt;
    q[1]++;
    threads[id].event->signaled = 1;
    hos_object_wake(threads[id].event);
}

static void raise_interrupt(int interrupt) /* (under hos_lock, no game code running: in a system call, or by hos_defer) */
{
    if (interrupt == PDC0 || interrupt == PDC1) {
        for (uint32_t id = 0; id < MAX_THREADS; id++)
            signal_for(id, interrupt);
        return;
    }
    if (rights != 0xFFFFFFFFu)
        signal_for(rights, interrupt);
}

/* a texture copy (Azahar's SwBlitter::TextureCopy): size bytes (16-byte aligned) from input to output, each side
   copying width bytes then skipping gap bytes (both in 16-byte units; a zero gap is contiguous). It was only
   acknowledged before. ORAS sends none through the opening and the title movie (counted, 6 November); a copy into a
   render target is reported by the renderer, whose image is not loaded back from memory yet. */
static void texture_copy(uint32_t from, uint32_t to, uint32_t size, uint32_t in_wg, uint32_t out_wg)
{
    uint32_t left = size & ~15u;
    const uint32_t in_gap = (in_wg >> 16) * 16, out_gap = (out_wg >> 16) * 16;
    const uint32_t in_w = in_gap ? (in_wg & 0xFFFF) * 16 : left, out_w = out_gap ? (out_wg & 0xFFFF) * 16 : left;
    if (!left || !in_w || !out_w) /* (the hardware freezes on these: nothing done) */
        return;
    uint8_t *s = hos_pointer(from), *d = hos_pointer(to);
    uint32_t in_left = in_w, out_left = out_w;
    while (left) {
        uint32_t n = in_left < out_left ? in_left : out_left;
        if (n > left)
            n = left;
        memmove(d, s, n);
        s += n;
        d += n;
        in_left -= n;
        out_left -= n;
        left -= n;
        if (!in_left) { in_left = in_w; s += in_gap; }
        if (!out_left) { out_left = out_w; d += out_gap; }
    }
    renderer_copied(to, (uint32_t)(d - (uint8_t*)hos_pointer(to)));
}

/* A display transfer (Azahar's SwBlitter::DisplayTransfer): an image from in to out (game addresses), its sizes
 * (in_size, out_size: width in the low 16 bits, height in the high) and flags (bit 0 flip vertically, 1 linear input,
 * 2 crop input lines, 5 no swizzle, 8-10 input format, 12-14 output format, 24-25 scaling): each output pixel read
 * from the input (8 x 8 Morton tiles on the tiled side), converted, box-filtered when scaled down. The render target
 * holding the input is written into memory first (renderer_flush, Azahar's FlushRegion). It was only shown on screen
 * before; the game also uses it to copy a rendered image into its memory (on the field: 128 x 128 RGBA4 from VRAM
 * into the linear heap, 7 November). */
/* A memory fill (Azahar's SwBlitter::MemoryFill): start to end (game addresses) with value, 24-bit (width bit 0: its
 * low three bytes r g b), 32-bit (bit 1) or 16-bit (its low half). Only the render targets were cleared before, the
 * memory never: a display transfer reading memory a fill had cleared (7 November) read what was there before. */
static void memory_fill(uint32_t start, uint32_t end, uint32_t value, uint32_t width_bits)
{
    if (end <= start)
        return;
    uint8_t *p = hos_pointer(start), *e = p + (end - start);
    if (width_bits & 1) {
        for (; p < e; p += 3) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); p[2] = (uint8_t)(value >> 16); }
    } else if (width_bits & 2) {
        for (uint32_t i = 0; i < (end - start) / 4; i++)
            memcpy(p + 4 * i, &value, 4);
    } else {
        const uint16_t v = (uint16_t)value;
        for (; p < e; p += 2)
            memcpy(p, &v, 2);
    }
}

static uint32_t morton(uint32_t x, uint32_t y, uint32_t bpp)
{
    static const uint32_t xlut[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const uint32_t ylut[8] = {0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a};
    return (xlut[x % 8] + ylut[y % 8] + (x & ~7u) * 8) * bpp;
}

static const uint32_t pixel_bytes[8] = {4, 3, 2, 2, 2, 0, 0, 0}; /* PixelFormat: RGBA8, RGB8, RGB565, RGB5A1, RGBA4 */

static void decode_pixel(uint32_t format, const uint8_t* p, uint32_t c[4])
{
    uint16_t v;
    switch (format) {
    case 0: c[0] = p[3]; c[1] = p[2]; c[2] = p[1]; c[3] = p[0]; return;
    case 1: c[0] = p[2]; c[1] = p[1]; c[2] = p[0]; c[3] = 255; return;
    case 2: memcpy(&v, p, 2); c[0] = (v >> 11 & 31) << 3 | (v >> 11 & 31) >> 2; c[1] = (v >> 5 & 63) << 2 | (v >> 5 & 63) >> 4;
            c[2] = (v & 31) << 3 | (v & 31) >> 2; c[3] = 255; return;
    case 3: memcpy(&v, p, 2); c[0] = (v >> 11 & 31) << 3 | (v >> 11 & 31) >> 2; c[1] = (v >> 6 & 31) << 3 | (v >> 6 & 31) >> 2;
            c[2] = (v >> 1 & 31) << 3 | (v >> 1 & 31) >> 2; c[3] = (v & 1) * 255; return;
    default: memcpy(&v, p, 2); c[0] = (v >> 12 & 15) * 17; c[1] = (v >> 8 & 15) * 17; c[2] = (v >> 4 & 15) * 17;
             c[3] = (v & 15) * 17; return;
    }
}

static void encode_pixel(uint32_t format, const uint32_t c[4], uint8_t* p)
{
    uint16_t v;
    switch (format) {
    case 0: p[3] = (uint8_t)c[0]; p[2] = (uint8_t)c[1]; p[1] = (uint8_t)c[2]; p[0] = (uint8_t)c[3]; return;
    case 1: p[2] = (uint8_t)c[0]; p[1] = (uint8_t)c[1]; p[0] = (uint8_t)c[2]; return;
    case 2: v = (uint16_t)((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3); break;
    case 3: v = (uint16_t)((c[0] >> 3) << 11 | (c[1] >> 3) << 6 | (c[2] >> 3) << 1 | c[3] >> 7); break;
    case 4: v = (uint16_t)((c[0] >> 4) << 12 | (c[1] >> 4) << 8 | (c[2] >> 4) << 4 | c[3] >> 4); break;
    default: return; /* (Azahar logs an unknown format and writes nothing) */
    }
    memcpy(p, &v, 2);
}

static void display_transfer(uint32_t in, uint32_t out, uint32_t in_size, uint32_t out_size, uint32_t flags)
{
    const uint32_t in_w = in_size & 0xFFFF, in_h = in_size >> 16, out_w0 = out_size & 0xFFFF, out_h0 = out_size >> 16;
    const uint32_t flip = flags & 1, linear = flags >> 1 & 1, crop = flags >> 2 & 1, no_swizzle = flags >> 5 & 1;
    const uint32_t in_fmt = flags >> 8 & 7, out_fmt = flags >> 12 & 7, scaling = flags >> 24 & 3;
    if (!in_w || !in_h || !out_w0 || !out_h0 || scaling > 2 || (linear && scaling) || !pixel_bytes[in_fmt])
        return; /* (Azahar: logged, nothing done) */
    const uint32_t sb = pixel_bytes[in_fmt], db = pixel_bytes[out_fmt] ? pixel_bytes[out_fmt] : 4;
    if (flip && crop) /* (flipping and cropping together skew the output on hardware: Azahar emulates it) */
        out += (in_w - out_w0) * (out_h0 - 1) * db;
    const uint32_t hs = scaling ? 1 : 0, vs = scaling == 2 ? 1 : 0;
    const uint32_t out_w = out_w0 >> hs, out_h = out_h0 >> vs;
    renderer_flush(in, in_w * in_h * sb);
    const uint8_t* src = hos_pointer(in);
    uint8_t* dst = hos_pointer(out);
    if (!src || !dst)
        return;
    for (uint32_t y = 0; y < out_h; y++)
        for (uint32_t x = 0; x < out_w; x++) {
            const uint32_t ix = x << hs, iy = y << vs, oy = flip ? out_h - y - 1 : y;
            uint32_t so, d;
            if (linear) {
                so = (ix + iy * in_w) * sb;
                d = no_swizzle ? (x + oy * out_w) * db : morton(x, oy, db) + (oy & ~7u) * out_w * db;
            } else {
                so = morton(ix, iy, sb) + (iy & ~7u) * in_w * sb;
                d = no_swizzle ? morton(x, oy, db) + (oy & ~7u) * out_w * db : (x + oy * out_w) * db;
            }
            uint32_t c[4];
            decode_pixel(in_fmt, src + so, c);
            if (scaling == 1) {
                uint32_t e[4];
                decode_pixel(in_fmt, src + so + sb, e);
                for (int k = 0; k < 4; k++) c[k] = (c[k] + e[k]) / 2;
            } else if (scaling == 2) {
                uint32_t e1[4], e2[4], e3[4];
                decode_pixel(in_fmt, src + so + sb, e1);
                decode_pixel(in_fmt, src + so + 2 * sb, e2);
                decode_pixel(in_fmt, src + so + 3 * sb, e3);
                for (int k = 0; k < 4; k++) c[k] = ((c[k] + e1[k]) + (e2[k] + e3[k])) / 4;
            }
            encode_pixel(out_fmt, c, dst + d);
        }
    renderer_copied(out, out_w * out_h * db);
}

void hos_gsp_report(void);

static void vblank(void)
{
    if (!memory)
        return;
    frames++;
    hos_defer(raise_interrupt, PDC0); /* (the clock's thread: written when no game code runs, hos_defer) */
    hos_defer(raise_interrupt, PDC1);
}

static void ok(HosRequest* q)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, HOS_OK);
}

static uint32_t thread_id(HosRequest* q)
{
    if (!q->session->service_data) /* (each session its thread id, from 0: Azahar's next_thread_id) */
        q->session->service_data = (void*)(uintptr_t)(1 + next_thread++);
    return (uint32_t)(uintptr_t)q->session->service_data - 1;
}

static void register_interrupt_relay_queue(HosRequest* q)
{
    uint32_t id = thread_id(q);
    if (!memory) {
        memory = hos_object_new(HOS_MEMORY_BLOCK);
        memory->block_size = 0x1000; /* (placed where the game maps it: MapMemoryBlock) */
        hos_clock_every(1000000000 / 60, vblank); /* (Azahar's FRAME_TICKS: a 59.83 Hz screen, here 60) */
        hos_clock_every(5000000000LL, hos_gsp_report);
    }
    if (id < MAX_THREADS) {
        threads[id].event = hos_handle_get(hos_in(q, 3), q->thread);
        threads[id].registered = 1;
    }
    uint32_t h = hos_handle_new(memory);
    hos_reply(q, 2, 2);
    hos_out(q, 1, first_initialization ? HOS_RESULT(519, 10, 0, 0) : HOS_OK); /* FirstInitialization */
    first_initialization = 0;
    hos_out(q, 2, id);
    hos_out_handles(q, 3, 0, &h, 1);
}

static void acquire_right(HosRequest* q)
{
    rights = thread_id(q);
    ok(q);
}

static void write_hw_regs(HosRequest* q, int masked)
{
    uint32_t reg = hos_in(q, 1), size = hos_in(q, 2), n, m;
    uint32_t data = hos_in_static(q, 3, &n), mask = masked ? hos_in_static(q, 5, &m) : 0;
    for (uint32_t i = 0; i + 4 <= size && reg + i < sizeof registers; i += 4) {
        uint32_t v = ld32(&q->thread->cpu, data + i), k = masked ? ld32(&q->thread->cpu, mask + i) : 0xFFFFFFFFu;
        registers[(reg + i) / 4] = (registers[(reg + i) / 4] & ~k) | (v & k);
    }
    ok(q);
}

static void write_hw_regs_plain(HosRequest* q) { write_hw_regs(q, 0); }
static void write_hw_regs_masked(HosRequest* q) { write_hw_regs(q, 1); }

/* one command run as the GPU runs it, then its interrupt raised (under hos_lock: locked says whether the caller holds
   it; the work itself needs it not, it touches only the game's memory and the renderer) */
static void run_command(const uint32_t* w, int locked)
{
    int interrupts[2], n = 0;
    switch (w[0] & 0xFF) {
    case 0: /* RequestDma: source, destination, size */
        memmove(hos_pointer(w[2]), hos_pointer(w[1]), w[3]);
        interrupts[n++] = DMA;
        break;
    case 1: /* SubmitCmdList: address, size */
        renderer_submit_list(w[1], w[2]);
        interrupts[n++] = P3D;
        break;
    case 2: /* MemoryFill: start1, value1, end1, start2, value2, end2, controls (each 16 bits: width in 8-9) */
        if (w[1]) {
            memory_fill(w[1], w[3], w[2], w[7] >> 8 & 3);
            renderer_fill(w[1], w[3], w[2], w[7] >> 8 & 3);
        }
        if (w[4]) {
            memory_fill(w[4], w[6], w[5], w[7] >> 24 & 3);
            renderer_fill(w[4], w[6], w[5], w[7] >> 24 & 3);
        }
        if (w[1])
            interrupts[n++] = PSC0;
        if (w[4])
            interrupts[n++] = PSC1;
        break;
    case 3: /* DisplayTransfer: input, output, sizes, flags */
    case 4: /* TextureCopy: input, output, size, input width / gap, output width / gap, flags */
        /* (either command is a texture copy when its flags say so, bit 3, as Azahar's MemoryTransfer decides: a
           DisplayTransfer's flags are its w[5], a TextureCopy's its w[6]) */
        if ((w[0] & 0xFF) == 4 ? w[6] >> 3 & 1 : w[5] >> 3 & 1)
            texture_copy(w[1], w[2], (w[0] & 0xFF) == 4 ? w[3] : 0, (w[0] & 0xFF) == 4 ? w[4] : 0,
                         (w[0] & 0xFF) == 4 ? w[5] : 0);
        else {
            display_transfer(w[1], w[2], w[3], w[4], w[5]);
            renderer_display(w[1]);
        }
        interrupts[n++] = PPF;
        break;
    default: /* CacheFlush */
        break;
    }
    if (!n)
        return;
    if (locked) { /* (in the game's system call) */
        for (int k = 0; k < n; k++)
            raise_interrupt(interrupts[k]);
        return;
    }
    pthread_mutex_lock(&hos_lock); /* (the GPU's thread: written when no game code runs, hos_defer) */
    for (int k = 0; k < n; k++)
        hos_defer(raise_interrupt, interrupts[k]);
    pthread_mutex_unlock(&hos_lock);
}

/* The GPU's own thread (AGENT_HANDOFF.md, next step 2 of 6 November). The commands were run inside the game's
 * TriggerCmdReqQueue, holding the kernel's lock and the game's one CPU: no other game thread ran, nor the clock (the
 * VBlanks, the DSP's frames: the DSP freeze of 6 November), while a frame was drawn. As on the 3DS, whose GPU works
 * beside the CPU, the commands now go in order to a thread of their own, which runs each and raises its interrupt
 * when it is done; the game waits for that interrupt where it already did. The order of all the commands is kept (a
 * DMA or a fill after a list runs after it). $HOS_GPU_SYNC=1 runs them in the call, as before (Azahar's way: it
 * answers at once). */
enum { GPU_QUEUE = 256 };
static uint32_t gpu_commands[GPU_QUEUE][8];
static unsigned gpu_head, gpu_tail; /* (taken at head, added at tail; equal: empty) */
static pthread_mutex_t gpu_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gpu_added = PTHREAD_COND_INITIALIZER, gpu_taken = PTHREAD_COND_INITIALIZER;

static void* gpu_thread(void* arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&gpu_mutex);
        while (gpu_head == gpu_tail)
            pthread_cond_wait(&gpu_added, &gpu_mutex);
        uint32_t w[8];
        memcpy(w, gpu_commands[gpu_head % GPU_QUEUE], sizeof w);
        pthread_mutex_unlock(&gpu_mutex);
        run_command(w, 0);
        pthread_mutex_lock(&gpu_mutex);
        gpu_head++; /* (taken only once done: a full queue's waiter, and gpu_idle, see it finished) */
        pthread_cond_broadcast(&gpu_taken);
        pthread_mutex_unlock(&gpu_mutex);
    }
    return NULL;
}

static int gpu_async(void)
{
    static int state = -1;
    if (state < 0) {
        state = !getenv("HOS_GPU_SYNC");
        pthread_t t;
        if (state && pthread_create(&t, NULL, gpu_thread, NULL))
            state = 0;
    }
    return state;
}

/* a command to the GPU's thread (called under hos_lock: let go while the queue is full) */
static void gpu_queue(const uint32_t* w)
{
    pthread_mutex_lock(&gpu_mutex);
    while (gpu_tail - gpu_head >= GPU_QUEUE) {
        pthread_mutex_unlock(&hos_lock); /* (the GPU's thread takes it to raise an interrupt) */
        pthread_cond_wait(&gpu_taken, &gpu_mutex);
        pthread_mutex_unlock(&gpu_mutex);
        pthread_mutex_lock(&hos_lock);
        pthread_mutex_lock(&gpu_mutex);
    }
    memcpy(gpu_commands[gpu_tail % GPU_QUEUE], w, 8 * 4);
    gpu_tail++;
    pthread_cond_signal(&gpu_added);
    pthread_mutex_unlock(&gpu_mutex);
}

/* the commands of the thread holding the right, as GSP takes them (index and count in the buffer's first word) */
static void trigger_cmd_req_queue(HosRequest* q)
{
    if (rights == 0xFFFFFFFFu || !memory || !memory->block_address) {
        ok(q);
        return;
    }
    uint8_t* buffer = shared(0x800 + 0x200 * rights);
    for (;;) {
        uint32_t head;
        memcpy(&head, buffer, 4);
        uint32_t index = head & 0xFF, count = (head >> 8) & 0xFF, status = (head >> 16) & 0xFF, stop = head >> 24;
        if (!count)
            break;
        if (stop) {
            head = (head & ~0xFF0000u) | 1u << 16; /* stopped */
            memcpy(buffer, &head, 4);
            break;
        }
        if (status == 1)
            break;
        const uint8_t* c = buffer + 0x20 + 0x20 * index;
        uint32_t w[8];
        memcpy(w, c, sizeof w);
        head = (head & ~0xFFFFu) | ((index + 1) % 0xF) | (count - 1) << 8;
        memcpy(buffer, &head, 4);
        if ((w[0] & 0xFF) == 1) {
            hos_gpu_trace_list(w[1], w[2]); /* (the test build's trace: the list as the game handed it over) */
            if (!command_lists++)
                fprintf(stderr, "hos: the game's first GPU command list: %08X, %u bytes (frame %llu)\n", w[1], w[2],
                        (unsigned long long)frames);
        }
        if (gpu_async())
            gpu_queue(w);
        else
            run_command(w, 1);
    }
    ok(q);
}

void hos_gsp_report(void)
{
    hos_gpu_trace_write();
    renderer_report();
    extern unsigned long long hos_dsp_frames_waited;
    fprintf(stderr, "hos: %llu GPU command lists, %llu VBlanks; audio frame attempts put off (the game not ready) %llu\n",
            (unsigned long long)command_lists, (unsigned long long)frames, hos_dsp_frames_waited);
}

static const HosCommandEntry commands[] = {
    {0x0001, write_hw_regs_plain, "WriteHWRegs"},
    {0x0002, write_hw_regs_masked, "WriteHWRegsWithMask"},
    {0x0008, ok, "FlushDataCache"},
    {0x000B, ok, "SetLcdForceBlack"},
    {0x000C, trigger_cmd_req_queue, "TriggerCmdReqQueue"},
    {0x0010, ok, "SetAxiConfigQoSMode"},
    {0x0013, register_interrupt_relay_queue, "RegisterInterruptRelayQueue"},
    {0x0016, acquire_right, "AcquireRight"},
    {0, NULL, NULL},
};

const HosService hos_gsp_gpu = {"gsp::Gpu", commands};
