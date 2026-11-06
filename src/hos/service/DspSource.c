/* The DSP's 24 sound sources as the game sees them: each audio frame (160 samples), the configuration the game wrote in
 * one of the two shared memory regions read, and each source's status written to the other, as Azahar's high-level DSP
 * does (audio_core/hle/source.cpp: ParseConfig, GenerateFrame, DequeueBuffer, GetCurrentStatus; hle.cpp: the regions).
 * A game times what follows a sound by these statuses (its play position, the buffers done): with none written, ORAS's
 * opening stood still at its first frame (5 November).
 *
 * Not done yet: decoding and mixing the samples (the sources' sound, step 5): a buffer's samples are counted, not
 * read. The count follows Azahar's interpolation (an output sample a step of rate_multiplier in the input, the input
 * consumed whole samples at a time), which is how its play position moves; filters, gains and the final mix are not
 * needed for the statuses. Structure offsets from Azahar's shared_memory.h (compiled, 5 November): a configuration is
 * 0xC0 bytes at 0x3D24 of a region, a status 0xC bytes at 0xD00; regions at 0x50000 and 0x70000 of the DSP's memory;
 * the region read is the one with the higher frame counter (at 0x7FFE), with Azahar's wraparound rule. */
#include "DspSource.h"

#include <string.h>

#include "../Memory.h"

enum { SOURCES = 24, QUEUE = 32, CONFIG = 0x3D24, CONFIG_SIZE = 0xC0, STATUS = 0xD00, STATUS_SIZE = 0xC };

typedef struct { uint32_t length, buffer_id, play_position; int looping, from_queue, played; } Buffer;

static struct {
    int enabled, buffer_update;
    uint32_t sync_count, current_id, last_id, length, position;
    int looping, has_current;
    float rate, phase;
    Buffer queue[QUEUE];
    int queued;
    Buffer current;
} source[SOURCES];

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
/* a u32_dsp: its two halves swapped */
static uint32_t rd_dsp(const uint8_t* p) { return (uint32_t)rd16(p) << 16 | rd16(p + 2); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void push(int s, Buffer b)
{
    if (source[s].queued < QUEUE)
        source[s].queue[source[s].queued++] = b;
}

/* the lowest buffer id first (Azahar's BufferOrder) */
static int pop(int s, Buffer* out)
{
    if (!source[s].queued)
        return 0;
    int best = 0;
    for (int k = 1; k < source[s].queued; k++)
        if (source[s].queue[k].buffer_id < source[s].queue[best].buffer_id)
            best = k;
    *out = source[s].queue[best];
    source[s].queue[best] = source[s].queue[--source[s].queued];
    return 1;
}

static void parse(int s, uint8_t* c)
{
    uint32_t dirty = rd32(c);
    if (!dirty)
        return;
    if (dirty & 1u << 29) /* reset */
        memset(&source[s], 0, sizeof source[s]);
    if (dirty & 1u << 4) /* partial reset: the queue emptied */
        source[s].queued = 0;
    if (dirty & 1u << 16)
        source[s].enabled = c[0xA0] != 0;
    if (dirty & 1u << 28)
        source[s].sync_count = rd16(c + 0xA2);
    if (dirty & 1u << 18) {
        float rate;
        memcpy(&rate, c + 0x34, 4);
        source[s].rate = rate > 0 ? rate : 1.0f;
    }
    uint32_t play_position = 0;
    if (dirty & 1u << 21)
        play_position = rd_dsp(c + 0xA4);
    if (dirty & 1u << 3) { /* partial embedded buffer: the current buffer's length and looping changed */
        source[s].length = rd_dsp(c + 0xB0);
        source[s].looping = rd16(c + 0xBC) >> 1 & 1;
        if (source[s].position > source[s].length)
            source[s].position = 0;
    }
    if (dirty & 1u << 30) { /* the embedded buffer */
        const uint32_t length = rd_dsp(c + 0xB0);
        if ((int32_t)length >= 0)
            push(s, (Buffer){length, rd16(c + 0xBE), play_position, rd16(c + 0xBC) >> 1 & 1, 0, 0});
    }
    if (dirty & 1u << 19) { /* the queue: buffers whose dirty bit is set */
        const uint16_t which = rd16(c + 0x4A);
        for (int i = 0; i < 4; i++)
            if (which >> i & 1) {
                const uint8_t* b = c + 0x4C + 0x14 * i;
                const uint32_t length = rd_dsp(b + 4);
                if ((int32_t)length >= 0)
                    push(s, (Buffer){length, rd16(b + 0x10), 0, b[0xF] != 0, 1, 0});
            }
        wr16(c + 0x4A, 0);
    }
    memset(c, 0, 4); /* (the DSP clears the dirty flags it has read) */
}

static int dequeue(int s)
{
    Buffer b;
    if (!pop(s, &b))
        return 0;
    source[s].position = b.played ? 0 : b.play_position;
    source[s].length = b.length;
    source[s].looping = b.looping;
    source[s].current_id = b.buffer_id;
    source[s].last_id = 0;
    source[s].buffer_update = b.from_queue && !b.played;
    source[s].has_current = source[s].position < b.length;
    if (b.looping) {
        b.played = 1;
        push(s, b);
    }
    return 1;
}

static void frame(int s)
{
    if (!source[s].has_current) {
        if (dequeue(s))
            return;
        source[s].enabled = 0;
        source[s].buffer_update = 1;
        source[s].last_id = source[s].current_id;
        source[s].current_id = 0;
        return;
    }
    /* 160 output samples, each a step of the rate in the input (Azahar's AudioInterp consumes whole samples) */
    for (int out = 0; out < 160; out++) {
        if (!source[s].has_current && !dequeue(s))
            break;
        source[s].phase += source[s].rate;
        while (source[s].phase >= 1.0f && source[s].has_current) {
            source[s].phase -= 1.0f;
            if (++source[s].position >= source[s].length)
                source[s].has_current = 0;
        }
    }
}

unsigned long long hos_dsp_frames_waited; /* attempts put off (a tick or a 0.25 ms retry, Dsp.c): the game had not written a new frame */

int hos_dsp_sources_tick(void)
{
    uint8_t* dsp = hos_pointer(HOS_DSP);
    uint8_t* r0 = dsp + 0x50000, *r1 = dsp + 0x70000;
    const uint16_t f0 = rd16(r0 + 0x7FFE), f1 = rd16(r1 + 0x7FFE);
    int read_first; /* Azahar's CurrentRegionIndex: the higher counter, 0xFFFF before 0 */
    if (f0 == 0xFFFF && f1 != 0xFFFE)
        read_first = 0;
    else if (f1 == 0xFFFF && f0 != 0xFFFE)
        read_first = 1;
    else
        read_first = f0 > f1 ? 0 : 1;
    /* a frame only once the game has written a new one (its counter moved). The DSP's frames and the game's alternate on
       the 3DS: the game always handles a frame's statuses within the DSP's 4.89 ms. Here a draw of ours holds the
       kernel lock for tens of ms and the clock then caught up by running frames back to back: the second one wrote
       the statuses again with current_buffer_id_dirty cleared, so the game never saw a buffer retire, kept it queued,
       later reused it and closed its source queue on itself (6 November: the opening's freeze, 00190C28 looping on a
       circular list; seen in the statuses the game read: current buffer 44, its queue's head 43, never the dirty flag).
       A slow frame of ours now delays the sound instead. */
    static int seen;
    static uint16_t last_counter;
    const uint16_t counter = read_first == 0 ? f0 : f1;
    if (seen && counter == last_counter) {
        hos_dsp_frames_waited++;
        return 0;
    }
    seen = 1;
    last_counter = counter;
    uint8_t* read = read_first == 0 ? r0 : r1;
    uint8_t* write = read_first == 0 ? r1 : r0;
    for (int s = 0; s < SOURCES; s++) {
        parse(s, read + CONFIG + CONFIG_SIZE * s);
        if (source[s].enabled)
            frame(s);
        uint8_t* st = write + STATUS + STATUS_SIZE * s;
        st[0] = (uint8_t)source[s].enabled;
        st[1] = (uint8_t)source[s].buffer_update;
        source[s].buffer_update = 0;
        wr16(st + 2, (uint16_t)source[s].sync_count);
        wr16(st + 4, (uint16_t)(source[s].position >> 16)); /* (u32_dsp: high half first) */
        wr16(st + 6, (uint16_t)source[s].position);
        wr16(st + 8, (uint16_t)source[s].current_id);
        wr16(st + 10, (uint16_t)source[s].last_id);
    }
    return 1;
}
