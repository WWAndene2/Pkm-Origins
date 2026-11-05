/* cecd:u: StreetPass (the CEC daemon), as Azahar answers it (service/cecd/cecd.cpp). Its data is the CEC system save
 * data (0x00010026), kept here in the extracted folder's cecd/ as fs:USER keeps the game's (FsHost.c), made as Azahar
 * makes it the first time (Module::Module: the CEC folder, eventlog.dat, CEC/MBoxList____). Done: OpenAndRead and
 * OpenAndWrite of the boxes' files (paths as GetCecDataPathTypeAsString). A write asking for the files' check (open mode
 * bit 4: Azahar's CheckAndUpdateFile, which rewrites the box lists) is not done yet: it stops the thread. Other commands
 * are added as the game calls them; one not here stops the thread and names itself (Ipc.c). */
#include "../Ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "FsHost.h"
#include "../Memory.h"

enum { MODULE_CEC = 66, NOT_AUTHORIZED = 1002, NO_DATA = 1007, SUMMARY_NOT_FOUND = 4, LEVEL_STATUS = 25 };

extern const char* hos_game_dir;
static char root[600];

static const char* archive(void)
{
    if (root[0])
        return root;
    snprintf(root, sizeof root, "%s/cecd", hos_game_dir);
    char cec[700];
    snprintf(cec, sizeof cec, "%s/CEC", root);
    FILE* f;
    if (fs_host_make_folders(root) == 0 && fs_host_create_directory(root, "/CEC") == HOS_OK) {
        /* the first time: eventlog.dat (0x30D54 bytes: 01 41 12, zero to 0x1000, then 0xDD) and MBoxList____
           (0x18C bytes: 68 68 00 00 01, then zero) */
        if (fs_host_open(root, "/eventlog.dat", 6, &f) == HOS_OK) {
            static uint8_t log[0x30D54];
            memset(log + 0x1000, 0xDD, sizeof log - 0x1000);
            log[0] = 0x01, log[1] = 0x41, log[2] = 0x12;
            fwrite(log, 1, sizeof log, f);
            fclose(f);
        }
        if (fs_host_open(root, "/CEC/MBoxList____", 6, &f) == HOS_OK) {
            uint8_t list[0x18C] = {0x68, 0x68, 0x00, 0x00, 0x01};
            fwrite(list, 1, sizeof list, f);
            fclose(f);
        }
    }
    return root;
}

/* a path type's file (Azahar's GetCecDataPathTypeAsString); 0 for the folders (10-13) and the messages (6, 7) */
static int path_of(uint32_t type, uint32_t program, char* out, size_t n)
{
    switch (type) {
    case 1: snprintf(out, n, "/CEC/MBoxList____"); return 1;
    case 2: snprintf(out, n, "/CEC/%08x/MBoxInfo____", program); return 1;
    case 3: snprintf(out, n, "/CEC/%08x/InBox___/BoxInfo_____", program); return 1;
    case 4: snprintf(out, n, "/CEC/%08x/OutBox__/BoxInfo_____", program); return 1;
    case 5: snprintf(out, n, "/CEC/%08x/OutBox__/OBIndex_____", program); return 1;
    case 6: case 7: case 10: case 11: case 12: case 13: return 0;
    default: snprintf(out, n, "/CEC/%08x/MBoxData.%03u", program, type - 100); return 1;
    }
}

static void open_and_read(HosRequest* q)
{
    uint32_t size = hos_in(q, 1), program = hos_in(q, 2), type = hos_in(q, 3), desc = hos_in(q, 7), at = hos_in(q, 8);
    char path[128];
    uint32_t result = HOS_OK, n = 0;
    if (type == 6 || type == 7) {
        fprintf(stderr, "hos: cecd:u OpenAndRead of a message (path type %u) not done\n", type);
        q->thread->cpu.fault = HOS_FAULT_SERVICE;
        return;
    }
    FILE* f;
    if (!path_of(type, program, path, sizeof path))
        result = HOS_RESULT(NOT_AUTHORIZED, MODULE_CEC, SUMMARY_NOT_FOUND, LEVEL_STATUS);
    else if (fs_host_open(archive(), path, 1, &f) != HOS_OK)
        result = HOS_RESULT(NO_DATA, MODULE_CEC, SUMMARY_NOT_FOUND, LEVEL_STATUS);
    else {
        uint8_t* buffer = calloc(size ? size : 1, 1);
        n = (uint32_t)fread(buffer, 1, size, f);
        memcpy(hos_pointer(at), buffer, size); /* (the whole buffer, as Azahar writes it) */
        free(buffer);
        fclose(f);
    }
    hos_reply(q, 2, 2);
    hos_out(q, 1, result);
    hos_out(q, 2, n);
    hos_out(q, 3, desc);
    hos_out(q, 4, at);
}

static void open_and_write(HosRequest* q)
{
    uint32_t size = hos_in(q, 1), program = hos_in(q, 2), type = hos_in(q, 3), mode = hos_in(q, 4);
    uint32_t desc = hos_in(q, 7), at = hos_in(q, 8);
    char path[128];
    uint32_t result = HOS_OK;
    if (type == 6 || type == 7 || (mode & 0x10)) {
        fprintf(stderr, "hos: cecd:u OpenAndWrite (path type %u, open mode %X) not done\n", type, mode);
        q->thread->cpu.fault = HOS_FAULT_SERVICE;
        return;
    }
    FILE* f;
    if (!path_of(type, program, path, sizeof path))
        result = HOS_RESULT(NOT_AUTHORIZED, MODULE_CEC, SUMMARY_NOT_FOUND, LEVEL_STATUS);
    else if (fs_host_open(archive(), path, 6, &f) != HOS_OK)
        result = HOS_RESULT(NO_DATA, MODULE_CEC, SUMMARY_NOT_FOUND, LEVEL_STATUS);
    else {
        fwrite(hos_pointer(at), 1, size, f);
        fflush(f);
        if (ftruncate(fileno(f), size)) {
        }
        fclose(f);
    }
    hos_reply(q, 1, 2);
    hos_out(q, 1, result);
    hos_out(q, 2, desc);
    hos_out(q, 3, at);
}

static const HosCommandEntry commands[] = {
    {0x0011, open_and_write, "OpenAndWrite"},
    {0x0012, open_and_read, "OpenAndRead"},
    {0, NULL, NULL},
};

const HosService hos_cecd_u = {"cecd:u", commands};
