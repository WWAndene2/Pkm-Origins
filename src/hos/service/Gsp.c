/* gsp::Gpu: the GPU service as Azahar's (service/gsp/gsp_gpu.cpp, video_core/gpu.cpp), with no GPU yet (the renderer
 * is step 4 of docs/RECOMPILATION_PLAN.md §3): the shared memory block (each thread's interrupt relay queue at
 * 0x40 its id, its command buffer at 0x800 + 0x200 its id), the GPU right, the hardware registers kept as written,
 * the commands the game queues taken from its buffer as GSP takes them and answered by the interrupts the GPU raises
 * when it is done (a command list: P3D; a memory fill: PSC0 / PSC1; a display transfer or texture copy: PPF; a DMA:
 * DMA, the copy made, as the game sees its result), at once rather than after the time the hardware takes; and the
 * VBlanks (PDC0 and PDC1) 60 times a second to every registered thread. Nothing is drawn: the command lists are
 * counted, the first one reported. */
#include "../Ipc.h"

#include <stdio.h>
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

static void signal(int interrupt)
{
    if (interrupt == PDC0 || interrupt == PDC1) {
        for (uint32_t id = 0; id < MAX_THREADS; id++)
            signal_for(id, interrupt);
        return;
    }
    if (rights != 0xFFFFFFFFu)
        signal_for(rights, interrupt);
}

void hos_gsp_report(void);

static void vblank(void)
{
    if (!memory)
        return;
    frames++;
    signal(PDC0);
    signal(PDC1);
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
        switch (w[0] & 0xFF) {
        case 0: /* RequestDma: source, destination, size */
            memmove(hos_pointer(w[2]), hos_pointer(w[1]), w[3]);
            signal(DMA);
            break;
        case 1: /* SubmitCmdList: address, size */
            hos_gpu_trace_list(w[1], w[2]);
            renderer_submit_list(w[1], w[2]);
            if (!command_lists++)
                fprintf(stderr, "hos: the game's first GPU command list: %08X, %u bytes (frame %llu)\n", w[1], w[2],
                        (unsigned long long)frames);
            signal(P3D);
            break;
        case 2: /* MemoryFill: start1, value1, end1, start2, value2, end2, controls (each 16 bits: width in 8-9) */
            if (w[1])
                renderer_fill(w[1], w[3], w[2], w[7] >> 8 & 3);
            if (w[4])
                renderer_fill(w[4], w[6], w[5], w[7] >> 24 & 3);
            if (w[1])
                signal(PSC0);
            if (w[4])
                signal(PSC1);
            break;
        case 3: /* DisplayTransfer: input, output, sizes, flags */
            renderer_display(w[1]);
            signal(PPF);
            break;
        case 4: /* TextureCopy */
            signal(PPF);
            break;
        default: /* CacheFlush */
            break;
        }
    }
    ok(q);
}

void hos_gsp_report(void)
{
    hos_gpu_trace_write();
    renderer_report();
    fprintf(stderr, "hos: %llu GPU command lists, %llu VBlanks\n", (unsigned long long)command_lists,
            (unsigned long long)frames);
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
