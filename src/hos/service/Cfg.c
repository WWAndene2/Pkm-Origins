/* cfg:u: the console's configuration, as Azahar answers (service/cfg/cfg.cpp): the blocks from the configuration
 * file (the 3DS's config savedata: a table of entries, block id, data or its offset, size, access flags; read from
 * the extracted folder's config, the player's console's or one an emulator made), each read only with the user-read
 * flag and at its own size; the region chosen as Azahar's auto-select does (the first the game's icon allows whose
 * languages hold the console's; else the first, with its first language); the transferable id a SHA-256 of the
 * console's unique id 2 and the game's salt. */
#include "../Ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../GameImage.h"
#include "../Memory.h"
#include "../Sha256.h"

enum { MODULE_CONFIG = 64, SUMMARY_WRONG_ARGUMENT = 8, LEVEL_PERMANENT = 27, USER_READ = 1 << 1 };
enum { NOT_AUTHORIZED = 1002, INVALID_SIZE = 1004, NOT_FOUND = 1018 };
enum { CONSOLE_UNIQUE_ID2 = 0x00090001, LANGUAGE = 0x000A0002 };

extern const char* hos_game_dir;
static uint8_t file[0x8000];
static int loaded, region = -1;

static int load(void)
{
    if (loaded)
        return loaded > 0;
    char path[4096];
    snprintf(path, sizeof path, "%s/config", hos_game_dir);
    FILE* f = fopen(path, "rb");
    loaded = f && fread(file, 1, sizeof file, f) > 0 ? 1 : -1;
    if (f)
        fclose(f);
    if (loaded < 0)
        fprintf(stderr, "hos: no console configuration (%s): the game's cfg requests cannot be answered\n", path);
    return loaded > 0;
}

/* the block's data (or NULL with the error in *result) */
static uint8_t* block(uint32_t id, uint32_t size, uint16_t access, uint32_t* result)
{
    uint16_t count;
    memcpy(&count, file, 2);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* e = file + 4 + 12 * i;
        uint32_t bid, off;
        uint16_t bsize, flags;
        memcpy(&bid, e, 4);
        memcpy(&off, e + 4, 4);
        memcpy(&bsize, e + 8, 2);
        memcpy(&flags, e + 10, 2);
        if (bid != id)
            continue;
        if (!(flags & access)) {
            *result = HOS_RESULT(NOT_AUTHORIZED, MODULE_CONFIG, SUMMARY_WRONG_ARGUMENT, LEVEL_PERMANENT);
            return NULL;
        }
        if (bsize != size) {
            *result = HOS_RESULT(INVALID_SIZE, MODULE_CONFIG, SUMMARY_WRONG_ARGUMENT, LEVEL_PERMANENT);
            return NULL;
        }
        *result = HOS_OK;
        return bsize <= 4 ? e + 4 : file + off;
    }
    *result = HOS_RESULT(NOT_FOUND, MODULE_CONFIG, SUMMARY_WRONG_ARGUMENT, LEVEL_PERMANENT);
    return NULL;
}

static int region_value(void)
{
    if (region >= 0)
        return region;
    static const uint8_t languages[7][8] = {{0}, {1, 2, 5, 9}, {1, 2, 3, 4, 5, 8, 9, 10}, {1, 2, 3, 4, 5, 8, 9, 10},
                                            {6}, {7}, {11}};
    static const int counts[7] = {1, 4, 8, 8, 1, 1, 1};
    uint32_t regions[7];
    int n = hos_game_regions(regions);
    uint32_t r;
    uint8_t* lang = block(LANGUAGE, 1, USER_READ, &r);
    region = n ? (int)regions[0] : 1;
    for (int i = 0; i < n && lang; i++)
        for (int k = 0; k < counts[regions[i]]; k++)
            if (languages[regions[i]][k] == *lang) {
                region = (int)regions[i];
                return region;
            }
    if (n && lang)
        *lang = languages[regions[0]][0]; /* (Azahar: the language adjusted to the region's first) */
    return region;
}

static void get_config(HosRequest* q)
{
    uint32_t size = hos_in(q, 1), id = hos_in(q, 2), desc = hos_in(q, 3), address = hos_in(q, 4);
    if (!load()) {
        q->thread->cpu.fault = HOS_FAULT_SERVICE;
        return;
    }
    if (id == LANGUAGE)
        region_value(); /* (the language as the region choice adjusted it) */
    uint32_t r;
    uint8_t* data = block(id, size, USER_READ, &r);
    if (data)
        memcpy(hos_pointer(address), data, size);
    else
        memset(hos_pointer(address), 0, size);
    hos_reply(q, 1, 2);
    hos_out(q, 1, r);
    hos_out(q, 2, desc);
    hos_out(q, 3, address);
}

static void get_region(HosRequest* q)
{
    if (!load()) {
        q->thread->cpu.fault = HOS_FAULT_SERVICE;
        return;
    }
    hos_reply(q, 2, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, (uint32_t)region_value());
}

static void get_transferable_id(HosRequest* q)
{
    if (!load()) {
        q->thread->cpu.fault = HOS_FAULT_SERVICE;
        return;
    }
    uint32_t salt = hos_in(q, 1) & 0x000FFFFF, r;
    uint8_t buffer[12], hash[32];
    uint8_t* id = block(CONSOLE_UNIQUE_ID2, 8, 0xE, &r); /* (Global access: user, system read and write) */
    hos_reply(q, 3, 0);
    hos_out(q, 1, r);
    if (!id) {
        hos_out(q, 2, 0);
        hos_out(q, 3, 0);
        return;
    }
    memcpy(buffer, id, 8);
    memcpy(buffer + 8, &salt, 4);
    hos_sha256(buffer, sizeof buffer, hash);
    uint32_t low, high;
    memcpy(&low, hash + 24, 4);
    memcpy(&high, hash + 28, 4);
    hos_out(q, 2, low);
    hos_out(q, 3, high);
}

static void is_coppacs_supported(HosRequest* q)
{
    if (!load()) {
        q->thread->cpu.fault = HOS_FAULT_SERVICE;
        return;
    }
    hos_reply(q, 2, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, region_value() == 1); /* (the Americas) */
}

static const HosCommandEntry commands[] = {
    {0x0001, get_config, "GetConfig"},
    {0x0002, get_region, "GetRegion"},
    {0x0003, get_transferable_id, "GetTransferableId"},
    {0x0004, is_coppacs_supported, "IsCoppacsSupported"},
    {0, NULL, NULL},
};

const HosService hos_cfg_u = {"cfg:u", commands};
