/* hid:USER: the buttons, touch screen and motion sensors, as Azahar gives them (service/hid/hid.cpp): a 0x1000-byte
 * shared memory block the service writes the input into and five one-shot events. The pad and touch screen are
 * updated 234 times a second as Azahar's UpdatePadCallback does (its SharedMem: the pad at 0x00, the touch screen at
 * 0xA8) (the pad's and touch's index and 8-entry histories,
 * the reset ticks, the first two events signaled); the motion sensors are not.
 *
 * The buttons come, for now, from a script ($HOS_INPUT, for runs with no player: "<seconds>:<buttons>,..." with
 * buttons joined by '+' among a b select start right left up down r l x y, or '-' for none; each holds until the
 * next, e.g. "20:a,20.2:-,30:start,30.2:-"). The touch screen is never touched. A player's input comes with step 5.
 */
#include "../Ipc.h"

#include <stdlib.h>
#include <string.h>

#include "../Memory.h"

static HosObject *memory, *events[5];
static struct { int64_t at; uint32_t buttons; } script[256];
static int script_count, pad_index, touch_index;
static int64_t start;

static uint32_t parse_buttons(const char* s, const char* end)
{
    static const char* names[12] = {"a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "x", "y"};
    uint32_t v = 0;
    while (s < end) {
        const char* e = s;
        while (e < end && *e != '+')
            e++;
        for (int i = 0; i < 12; i++)
            if ((size_t)(e - s) == strlen(names[i]) && !strncmp(s, names[i], (size_t)(e - s)))
                v |= 1u << i;
        s = e + 1;
    }
    return v;
}

static void read_script(void)
{
    const char* s = getenv("HOS_INPUT");
    while (s && *s && script_count < 256) {
        char* e;
        double t = strtod(s, &e);
        if (*e != ':')
            break;
        const char* b = e + 1;
        const char* stop = strchr(b, ',');
        if (!stop)
            stop = b + strlen(b);
        script[script_count].at = (int64_t)(t * 1e9);
        script[script_count++].buttons = parse_buttons(b, stop);
        s = *stop ? stop + 1 : stop;
    }
}

static uint32_t buttons_now(void)
{
    int64_t t = hos_now_ns() - start;
    uint32_t v = 0;
    for (int i = 0; i < script_count && script[i].at <= t; i++)
        v = script[i].buttons;
    return v;
}

static void w32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t r32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* (the clock thread, under hos_lock, 234 times a second) */
static void update(void)
{
    if (!memory->block_address)
        return;
    uint8_t* m = hos_pointer(memory->block_address);
    uint32_t state = buttons_now();
    /* the pad: 0x00 reset ticks, 0x08 the previous, 0x10 index, 0x18 3D slider, 0x1C state, 0x28 8 entries of 16 */
    pad_index = (pad_index + 1) % 8;
    uint32_t old = r32(m + 0x28 + 16 * ((pad_index + 7) % 8));
    uint8_t* e = m + 0x28 + 16 * pad_index;
    w32(m + 0x1C, state);
    w32(m + 0x10, (uint32_t)pad_index);
    w32(e, state);
    w32(e + 4, (state ^ old) & state);
    w32(e + 8, (state ^ old) & old);
    w32(e + 12, 0); /* (circle pad x, y: centred) */
    if (pad_index == 0) {
        memcpy(m + 0x08, m + 0x00, 8);
        uint64_t ticks = hos_ticks();
        memcpy(m + 0x00, &ticks, 8);
    }
    /* the touch screen: 0xA8 reset ticks, 0xB0 the previous, 0xB8 index, 0xC8 8 entries of 8 (never touched) */
    touch_index = (touch_index + 1) % 8;
    w32(m + 0xB8, (uint32_t)touch_index);
    memset(m + 0xC8 + 8 * touch_index, 0, 8);
    if (touch_index == 0) {
        memcpy(m + 0xB0, m + 0xA8, 8);
        uint64_t ticks = hos_ticks();
        memcpy(m + 0xA8, &ticks, 8);
    }
    for (int i = 0; i < 2; i++) {
        events[i]->signaled = 1;
        hos_object_wake(events[i]);
    }
}

HosObject* hos_hid_memory(void)
{
    if (!memory) {
        memory = hos_object_new(HOS_MEMORY_BLOCK);
        memory->block_size = 0x1000;
        for (int i = 0; i < 5; i++)
            events[i] = hos_object_new(HOS_EVENT);
        read_script();
        start = hos_now_ns();
        hos_clock_every(1000000000LL / 234, update); /* (Azahar's pad_update_ticks) */
    }
    return memory;
}

static void get_ipc_handles(HosRequest* q)
{
    hos_hid_memory();
    uint32_t h[6] = {hos_handle_new(memory)};
    for (int i = 0; i < 5; i++)
        h[i + 1] = hos_handle_new(events[i]);
    hos_reply(q, 1, 7);
    hos_out(q, 1, HOS_OK);
    hos_out_handles(q, 2, 0, h, 6);
}

static const HosCommandEntry commands[] = {
    {0x000A, get_ipc_handles, "GetIPCHandles"},
    {0, NULL, NULL},
};

const HosService hos_hid_user = {"hid:USER", commands};
