/* APT (APT:A, APT:U): the applet manager, as an application started alone (no HOME menu) sees it in Azahar
 * (service/apt/apt.cpp, applet_manager.cpp): GetLockHandle gives the APT lock (a mutex) and its attributes back;
 * Initialize registers the application, makes it the active applet, enables it and sends it the Wakeup parameter
 * (from no one), signaling its parameter event; Glance / ReceiveParameter give that parameter (ReceiveParameter
 * clears it); AppletUtility answers success (command 6, TryLockTransition, true: Azahar's stub). The shared font: from
 * the player's own console (shared_font.bin in the extracted folder: a 0x80-byte header and the decompressed BCFNT, as
 * Azahar's legacy loader reads it), relocated to where it is mapped; without one, Azahar's error. */
#include "../Ipc.h"

#include <stdio.h>
#include <string.h>

#include "../Memory.h"

enum { APPLICATION = 0x300, SIGNAL_WAKEUP = 1 };
enum { MODULE_APPLET = 51, SUMMARY_INVALID_STATE = 5, SUMMARY_NOT_FOUND = 4, LEVEL_STATUS = 25 };

static HosObject *lock, *notification_event, *parameter_event;
static struct { int pending; uint32_t sender, destination, signal; } parameter;
static int registered;

static void objects(void)
{
    if (lock)
        return;
    lock = hos_object_new(HOS_MUTEX);
    notification_event = hos_object_new(HOS_EVENT); /* (both one-shot: applet_manager.cpp) */
    parameter_event = hos_object_new(HOS_EVENT);
}

static void send_parameter(uint32_t sender, uint32_t destination, uint32_t signal)
{
    parameter.pending = 1;
    parameter.sender = sender;
    parameter.destination = destination;
    parameter.signal = signal;
    parameter_event->signaled = 1;
    hos_object_wake(parameter_event);
}

static void get_lock_handle(HosRequest* q)
{
    objects();
    uint32_t attributes = hos_in(q, 1); /* (an application's position is no library's: kept as it is) */
    uint32_t h = hos_handle_new(lock);
    hos_reply(q, 3, 2);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, attributes);
    hos_out(q, 3, 0); /* state */
    hos_out_handles(q, 4, 0, &h, 1);
}

static void initialize(HosRequest* q)
{
    objects();
    uint32_t app_id = hos_in(q, 1);
    if (registered) {
        hos_reply(q, 1, 0);
        hos_out(q, 1, HOS_RESULT(1020, MODULE_APPLET, SUMMARY_INVALID_STATE, LEVEL_STATUS)); /* AlreadyExists */
        return;
    }
    registered = 1;
    send_parameter(0, app_id, SIGNAL_WAKEUP); /* (the first applet: active, enabled, woken) */
    uint32_t h[2] = {hos_handle_new(notification_event), hos_handle_new(parameter_event)};
    hos_reply(q, 1, 3);
    hos_out(q, 1, HOS_OK);
    hos_out_handles(q, 2, 0, h, 2);
}

static void ok(HosRequest* q)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, HOS_OK);
}

static void parameter_reply(HosRequest* q, int receive)
{
    uint32_t app_id = hos_in(q, 1), buffer_size = hos_in(q, 2);
    if (!parameter.pending || parameter.destination != app_id) {
        hos_reply(q, 1, 0);
        hos_out(q, 1, !parameter.pending ? HOS_RESULT(1007, MODULE_APPLET, SUMMARY_INVALID_STATE, LEVEL_STATUS)
                                         : HOS_RESULT(1018, MODULE_APPLET, SUMMARY_NOT_FOUND, LEVEL_STATUS));
        return;
    }
    hos_reply(q, 4, 4);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, parameter.sender);
    hos_out(q, 3, parameter.signal);
    hos_out(q, 4, 0); /* the parameter's size: the Wakeup carries none */
    uint32_t none = 0;
    hos_out_handles(q, 5, 1, &none, 1); /* (its object: none) */
    static uint8_t zeros[0x1000];
    hos_out_static(q, 7, 0, zeros, buffer_size < sizeof zeros ? buffer_size : sizeof zeros);
    if (receive)
        parameter.pending = 0;
}

static void glance_parameter(HosRequest* q) { parameter_reply(q, 0); }
static void receive_parameter(HosRequest* q) { parameter_reply(q, 1); }

static void applet_utility(HosRequest* q)
{
    uint32_t command = hos_in(q, 1), output_size = hos_in(q, 3);
    static uint8_t out[0x1000];
    memset(out, 0, sizeof out);
    if (command == 6 && output_size > 0)
        out[0] = 1;
    hos_reply(q, 2, 2);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, HOS_OK);
    hos_out_static(q, 3, 0, out, output_size < sizeof out ? output_size : sizeof out);
}

/* the font's internal offsets moved to its address (Azahar's apt/bcfnt/bcfnt.cpp, RelocateSharedFont): the base the
   file assumed found from FINF's offsets against where the first CMAP / CWDH / TGLP are, then every offset moved */
static void relocate_font(uint8_t* font, uint32_t address)
{
    uint16_t header_size;
    uint32_t blocks;
    memcpy(&header_size, font + 0x80 + 6, 2);
    memcpy(&blocks, font + 0x80 + 0x10, 4);
    uint32_t assumed_cmap = 0, first_cmap = 0, at = 0x80u + header_size;
    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t size;
        memcpy(&size, font + at + 4, 4);
        if (!first_cmap && !memcmp(font + at, "CMAP", 4))
            first_cmap = at;
        else if (!memcmp(font + at, "FINF", 4)) {
            memcpy(&assumed_cmap, font + at + 0x18, 4); /* (FINF: tglp at + 0x10, cwdh + 0x14, cmap + 0x18) */
            assumed_cmap -= 8;
        }
        at += size;
    }
    uint32_t offset = address - (assumed_cmap - first_cmap);
    at = 0x80u + header_size;
    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t size, v;
        memcpy(&size, font + at + 4, 4);
        static const struct { const char* magic; int fields[3]; int keep_zero; } moved[] = {
            {"FINF", {0x10, 0x14, 0x18}, 0}, {"CMAP", {0x10, 0, 0}, 1}, {"CWDH", {0x0C, 0, 0}, 1}, {"TGLP", {0x1C, 0, 0}, 0}};
        for (int k = 0; k < 4; k++)
            if (!memcmp(font + at, moved[k].magic, 4))
                for (int f = 0; f < 3 && moved[k].fields[f]; f++) {
                    memcpy(&v, font + at + moved[k].fields[f], 4);
                    if (v || !moved[k].keep_zero) /* (a next-section offset of 0 means none) */
                        v += offset;
                    memcpy(font + at + moved[k].fields[f], &v, 4);
                }
        at += size;
    }
}

static void get_shared_font(HosRequest* q)
{
    static HosObject* font;
    if (!font) {
        extern const char* hos_game_dir;
        char path[4096];
        snprintf(path, sizeof path, "%s/shared_font.bin", hos_game_dir);
        FILE* f = fopen(path, "rb");
        if (!f) {
            /* (Azahar's answer without a font: -1, no address, no handle) */
            fprintf(stderr, "hos: no system font (%s: tools/make_shared_font.py makes it)\n", path);
            hos_reply(q, 2, 2);
            hos_out(q, 1, 0xFFFFFFFFu);
            hos_out(q, 2, 0);
            uint32_t none = 0;
            hos_out_handles(q, 3, 0, &none, 1);
            return;
        }
        font = hos_object_new(HOS_MEMORY_BLOCK);
        font->block_size = 0x332000; /* (Azahar's APT:SharedFont block: 3,272 KiB) */
        font->block_address = hos_memory_block_home(font->block_size);
        hos_memory_map(font->block_address, font->block_size);
        uint8_t* p = hos_pointer(font->block_address);
        size_t n = fread(p, 1, font->block_size, f);
        fclose(f);
        (void)n;
        relocate_font(p, font->block_address);
    }
    uint32_t h = hos_handle_new(font);
    hos_reply(q, 2, 2);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, font->block_address);
    hos_out_handles(q, 3, 0, &h, 1);
}

static const HosCommandEntry commands[] = {
    {0x0001, get_lock_handle, "GetLockHandle"},
    {0x0002, initialize, "Initialize"},
    {0x0003, ok, "Enable"},
    {0x000D, receive_parameter, "ReceiveParameter"},
    {0x000E, glance_parameter, "GlanceParameter"},
    {0x0043, ok, "NotifyToWait"},
    {0x0044, get_shared_font, "GetSharedFont"},
    {0x004B, applet_utility, "AppletUtility"},
    {0x0055, ok, "SetScreenCapturePostPermission"},
    {0, NULL, NULL},
};

const HosService hos_apt_a = {"APT:A", commands};
const HosService hos_apt_u = {"APT:U", commands};
