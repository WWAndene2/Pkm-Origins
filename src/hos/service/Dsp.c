/* dsp::DSP: the sound processor, as Azahar's high-level DSP has it (service/dsp/dsp_dsp.cpp, audio_core/hle/hle.cpp),
 * but silent: its pipes, its interrupts, the addresses of the structures it shares with the game (in its memory at
 * 0x1FF00000), and its audio frame tick (every 160 samples at 32,728 Hz: 4.89 ms), which signals the game's audio
 * event as Azahar's does, after writing the sources' statuses (DspSource.c). What the tick does not do yet: mix the
 * game's sound sources and write the mixed samples back (Azahar's GenerateCurrentFrame), and play them: step 5 (docs/RECOMPILATION_PLAN.md §3). The
 * binary pipe (the AAC decoder) is not done: a request on it stops the thread.
 */
#include "../Ipc.h"

#include <stdio.h>
#include <string.h>

#include "../Memory.h"
#include "DspSource.h"

enum { PIPES = 8, PIPE_AUDIO = 2, PIPE_BINARY = 3, INTERRUPT_PIPE = 2 };
enum { OFF, ON, SLEEPING };

static int state = OFF, ticking;
static uint8_t pipe_data[PIPES][64];
static uint32_t pipe_size[PIPES];
static HosObject* interrupt[2];          /* the interrupts "zero" and "one" */
static HosObject* pipe_interrupt[PIPES]; /* a pipe's interrupt */
static HosObject* semaphore_event;

static void ok(HosRequest* q)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, HOS_OK);
}

static void signal_event(HosObject* e)
{
    if (e) {
        e->signaled = 1;
        hos_object_wake(e);
    }
}

static void write_u16(int pipe, uint16_t v)
{
    if (pipe_size[pipe] + 2 <= sizeof pipe_data[pipe]) {
        pipe_data[pipe][pipe_size[pipe]++] = (uint8_t)v;
        pipe_data[pipe][pipe_size[pipe]++] = (uint8_t)(v >> 8);
    }
}

/* the 15 structures' addresses in the DSP's memory (16-bit words from 0x8000), as Azahar's SharedMemory lays them
   out: frame counter, source configurations and statuses, ADPCM coefficients, DSP configuration and status, final
   samples, intermediate mix samples, compressor, debug, and five unknown ones */
static const uint16_t structures[15] = {0x7FFE, 0x3D24, 0x0D00, 0x4F24, 0x2860, 0x0800, 0x0A80, 0x2924,
                                        0x0E20, 0x0820, 0x5224, 0x5424, 0x55A4, 0x58A4, 0x58B8};

static void write_structure_addresses(void)
{
    write_u16(PIPE_AUDIO, 15);
    for (int i = 0; i < 15; i++)
        write_u16(PIPE_AUDIO, (uint16_t)(0x8000 + structures[i] / 2));
    signal_event(pipe_interrupt[PIPE_AUDIO]);
}

static void tick(void)
{
    if (state == ON) {
        hos_dsp_sources_tick(); /* the sources' statuses (DspSource.c), as Azahar's GenerateCurrentFrame writes them */
        signal_event(pipe_interrupt[PIPE_AUDIO]);
    }
}

static void recv_data(HosRequest* q)
{
    hos_reply(q, 2, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, state == ON ? 0 : 1); /* (read after a shutdown or sleep request: 1 once done) */
}

static void recv_data_is_ready(HosRequest* q)
{
    hos_reply(q, 2, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, 1);
}

static void convert_address(HosRequest* q)
{
    uint32_t address = hos_in(q, 1);
    hos_reply(q, 2, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, (address << 1) + HOS_DSP + 0x40000);
}

static void write_process_pipe(HosRequest* q)
{
    uint32_t channel = hos_in(q, 1), size;
    uint32_t at = hos_in_static(q, 3, &size);
    uint8_t buffer[16] = {0};
    memcpy(buffer, hos_pointer(at), size < sizeof buffer ? size : sizeof buffer);
    if (channel == PIPE_AUDIO && size >= 4) {
        switch (buffer[0]) {
        case 0: /* initialize */
        case 2: /* wake up */
            write_structure_addresses();
            state = ON;
            break;
        case 1: /* shut down */
            state = OFF;
            break;
        case 3: /* sleep */
            write_structure_addresses();
            state = SLEEPING;
            break;
        default:
            state = OFF;
        }
        if (!ticking) {
            ticking = 1;
            hos_clock_every(1310720LL * 1000000000 / 268111856, tick); /* (Azahar's audio_frame_ticks: 4.89 ms) */
        }
    } else {
        fprintf(stderr, "hos: dsp::DSP WriteProcessPipe on pipe %u (%u bytes) not done\n", channel, size);
        q->thread->cpu.fault = HOS_FAULT_SERVICE;
        return;
    }
    ok(q);
}

static void read_pipe_common(HosRequest* q, int if_possible)
{
    uint32_t channel = hos_in(q, 1) % PIPES, size = hos_in(q, 3) & 0xFFFF;
    uint8_t out[64];
    uint32_t n = 0;
    if (pipe_size[channel] >= size) {
        n = size < sizeof out ? size : sizeof out;
        memcpy(out, pipe_data[channel], n);
        memmove(pipe_data[channel], pipe_data[channel] + n, pipe_size[channel] - n);
        pipe_size[channel] -= n;
    } else if (!if_possible) {
        fprintf(stderr, "hos: dsp::DSP ReadPipe of %u bytes, %u in the pipe (the hardware hangs)\n", size,
                pipe_size[channel]);
        q->thread->cpu.fault = HOS_FAULT_SERVICE;
        return;
    }
    if (if_possible) {
        hos_reply(q, 2, 2);
        hos_out(q, 1, HOS_OK);
        hos_out(q, 2, n);
        hos_out_static(q, 3, 0, out, n);
    } else {
        hos_reply(q, 1, 2);
        hos_out(q, 1, HOS_OK);
        hos_out_static(q, 2, 0, out, n);
    }
}

static void read_pipe(HosRequest* q)
{
    read_pipe_common(q, 0);
}

static void read_pipe_if_possible(HosRequest* q)
{
    read_pipe_common(q, 1);
}

static void get_pipe_readable_size(HosRequest* q)
{
    uint32_t channel = hos_in(q, 1) % PIPES;
    hos_reply(q, 2, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, pipe_size[channel]);
}

static void load_component(HosRequest* q)
{
    uint32_t descriptor = hos_in(q, 4), address = hos_in(q, 5);
    if (hos_memory_map(HOS_DSP, 0x80000) == 0)
        memset(hos_pointer(HOS_DSP), 0, 0x80000);
    hos_reply(q, 2, 2);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, 1); /* (loaded) */
    hos_out(q, 3, descriptor);
    hos_out(q, 4, address);
}

static void register_interrupt_events(HosRequest* q)
{
    uint32_t type = hos_in(q, 1), channel = hos_in(q, 2) % PIPES;
    HosObject* e = hos_in(q, 4) ? hos_handle_get(hos_in(q, 4), q->thread) : NULL;
    if (type == INTERRUPT_PIPE)
        pipe_interrupt[channel] = e;
    else if (type < 2)
        interrupt[type] = e;
    ok(q);
}

static void get_semaphore_event_handle(HosRequest* q)
{
    if (!semaphore_event) {
        semaphore_event = hos_object_new(HOS_EVENT);
        semaphore_event->reset = HOS_RESET_ONESHOT;
        strcpy(semaphore_event->name, "DSP sema");
    }
    uint32_t h = hos_handle_new(semaphore_event);
    hos_reply(q, 1, 2);
    hos_out(q, 1, HOS_OK);
    hos_out_handles(q, 2, 0, &h, 1);
}

static void get_headphone_status(HosRequest* q)
{
    hos_reply(q, 2, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, 0); /* (not inserted) */
}

static const HosCommandEntry commands[] = {
    {0x0001, recv_data, "RecvData"},
    {0x0002, recv_data_is_ready, "RecvDataIsReady"},
    {0x0007, ok, "SetSemaphore"},
    {0x000C, convert_address, "ConvertProcessAddressFromDspDram"},
    {0x000D, write_process_pipe, "WriteProcessPipe"},
    {0x000E, read_pipe, "ReadPipe"},
    {0x000F, get_pipe_readable_size, "GetPipeReadableSize"},
    {0x0010, read_pipe_if_possible, "ReadPipeIfPossible"},
    {0x0011, load_component, "LoadComponent"},
    {0x0012, ok, "UnloadComponent"},
    {0x0013, ok, "FlushDataCache"},
    {0x0014, ok, "InvalidateDCache"},
    {0x0015, register_interrupt_events, "RegisterInterruptEvents"},
    {0x0016, get_semaphore_event_handle, "GetSemaphoreEventHandle"},
    {0x0017, ok, "SetSemaphoreMask"},
    {0x001F, get_headphone_status, "GetHeadphoneStatus"},
    {0x0020, ok, "ForceHeadphoneOut"},
    {0, NULL, NULL},
};

const HosService hos_dsp_dsp = {"dsp::DSP", commands};
