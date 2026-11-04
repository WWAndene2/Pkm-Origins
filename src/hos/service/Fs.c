/* fs:USER and its file sessions, as Azahar answers (service/fs/fs_user.cpp, file.cpp, file_sys/): archives opened by
 * id and path, files in them read through sessions. Done so far: the game's own RomFS (archive 0x3, SelfNCCH, file
 * path type 0: the RomFS's level-3 data from the player's .3ds). Any other archive or path prints what the game asked
 * and stops the thread: what it needs is added from that, never guessed. */
#include "../Ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../GameImage.h"
#include "../Memory.h"

enum { MODULE_FS = 17, SUMMARY_NOT_FOUND = 4, SUMMARY_WRONG_ARGUMENT = 8, LEVEL_STATUS = 25, LEVEL_PERMANENT = 27 };
enum { ARCHIVE_SELF_NCCH = 0x3 };

typedef struct {
    int kind; /* 1: the RomFS */
    uint64_t size;
    uint32_t priority;
} FsFile;

extern const HosService hos_fs_file;
static uint32_t priority;
static uint64_t next_archive = 1;
static struct { uint64_t handle; uint32_t id; } archives[32];

static void stop(HosRequest* q, const char* what, uint32_t archive, uint32_t type, uint32_t size, uint32_t address)
{
    fprintf(stderr, "hos: fs:USER %s: archive %X, path type %u, %u bytes:", what, archive, type, size);
    for (uint32_t i = 0; i < size && i < 64; i++)
        fprintf(stderr, " %02X", ld8(&q->thread->cpu, address + i));
    fprintf(stderr, " (not done)\n");
    q->thread->cpu.fault = HOS_FAULT_SERVICE;
}

static void ok(HosRequest* q)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, HOS_OK);
}

static uint32_t open_session(FsFile f)
{
    FsFile* p = malloc(sizeof *p);
    *p = f;
    uint32_t h = hos_session_open(&hos_fs_file);
    hos_handle_get(h, NULL)->service_data = p;
    return h;
}

/* a file in an archive by its path; 0 when this archive / path is not done (the thread then stopped) */
static int open_in(HosRequest* q, uint32_t archive, uint32_t type, uint32_t size, uint32_t address, uint32_t* handle)
{
    if (archive == ARCHIVE_SELF_NCCH && type == 2 && size == 12 && ld32(&q->thread->cpu, address) == 0) {
        FsFile f = {1, hos_romfs_size(), 0};
        *handle = open_session(f);
        return 1;
    }
    stop(q, "open file", archive, type, size, address);
    return 0;
}

static void open_file_directly(HosRequest* q)
{
    uint32_t archive = hos_in(q, 2), atype = hos_in(q, 3), ftype = hos_in(q, 5), fsize = hos_in(q, 6);
    uint32_t asize, fbytes;
    uint32_t aaddr = hos_in_static(q, 9, &asize), faddr = hos_in_static(q, 11, &fbytes);
    if (archive != ARCHIVE_SELF_NCCH || atype != 1) {
        stop(q, "open archive (directly)", archive, atype, asize, aaddr);
        return;
    }
    uint32_t h;
    if (!open_in(q, archive, ftype, fsize, faddr, &h))
        return;
    hos_reply(q, 1, 2);
    hos_out(q, 1, HOS_OK);
    hos_out_handles(q, 2, 1, &h, 1);
}

static void open_archive(HosRequest* q)
{
    uint32_t archive = hos_in(q, 1), type = hos_in(q, 2), size;
    uint32_t address = hos_in_static(q, 4, &size);
    if (archive != ARCHIVE_SELF_NCCH || type != 1) {
        stop(q, "open archive", archive, type, size, address);
        return;
    }
    for (int i = 0; i < 32; i++)
        if (!archives[i].handle) {
            archives[i].handle = next_archive++;
            archives[i].id = archive;
            hos_reply(q, 3, 0);
            hos_out(q, 1, HOS_OK);
            hos_out(q, 2, (uint32_t)archives[i].handle);
            hos_out(q, 3, (uint32_t)(archives[i].handle >> 32));
            return;
        }
}

static uint32_t archive_of(uint64_t handle)
{
    for (int i = 0; i < 32; i++)
        if (archives[i].handle == handle)
            return archives[i].id;
    return 0xFFFFFFFFu;
}

static void open_file(HosRequest* q)
{
    uint64_t handle = (uint64_t)hos_in(q, 3) << 32 | hos_in(q, 2);
    uint32_t type = hos_in(q, 4), size = hos_in(q, 5), bytes;
    uint32_t address = hos_in_static(q, 8, &bytes);
    uint32_t archive = archive_of(handle);
    if (archive == 0xFFFFFFFFu) {
        hos_reply(q, 1, 2);
        hos_out(q, 1, HOS_RESULT(101, MODULE_FS, SUMMARY_NOT_FOUND, LEVEL_STATUS)); /* InvalidArchiveHandle */
        uint32_t none = 0;
        hos_out_handles(q, 2, 1, &none, 1);
        return;
    }
    uint32_t h;
    if (!open_in(q, archive, type, size, address, &h))
        return;
    hos_reply(q, 1, 2);
    hos_out(q, 1, HOS_OK);
    hos_out_handles(q, 2, 1, &h, 1);
}

static void close_archive(HosRequest* q)
{
    uint64_t handle = (uint64_t)hos_in(q, 2) << 32 | hos_in(q, 1);
    for (int i = 0; i < 32; i++)
        if (archives[i].handle == handle) {
            archives[i].handle = 0;
            ok(q);
            return;
        }
    hos_reply(q, 1, 0);
    hos_out(q, 1, HOS_RESULT(101, MODULE_FS, SUMMARY_NOT_FOUND, LEVEL_STATUS));
}

static void set_priority(HosRequest* q)
{
    priority = hos_in(q, 1);
    ok(q);
}

static void get_priority(HosRequest* q)
{
    hos_reply(q, 2, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, priority);
}

static const HosCommandEntry commands[] = {
    {0x0801, ok, "Initialize"},
    {0x0802, open_file, "OpenFile"},
    {0x0803, open_file_directly, "OpenFileDirectly"},
    {0x080C, open_archive, "OpenArchive"},
    {0x080E, close_archive, "CloseArchive"},
    {0x0861, ok, "InitializeWithSdkVersion"},
    {0x0862, set_priority, "SetPriority"},
    {0x0863, get_priority, "GetPriority"},
    {0, NULL, NULL},
};

const HosService hos_fs_user = {"fs:USER", commands};

/* the file sessions (file.cpp) */
static FsFile* file_of(HosRequest* q) { return q->session->service_data; }

static void file_read(HosRequest* q)
{
    FsFile* f = file_of(q);
    uint64_t offset = (uint64_t)hos_in(q, 2) << 32 | hos_in(q, 1);
    uint32_t length = hos_in(q, 3), desc = hos_in(q, 4), address = hos_in(q, 5);
    size_t n = f->kind == 1 ? hos_romfs_read(offset, hos_pointer(address), length) : 0;
    hos_reply(q, 2, 2);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, (uint32_t)n);
    hos_out(q, 3, desc);
    hos_out(q, 4, address);
}

static void file_get_size(HosRequest* q)
{
    FsFile* f = file_of(q);
    hos_reply(q, 3, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, (uint32_t)f->size);
    hos_out(q, 3, (uint32_t)(f->size >> 32));
}

static void file_close(HosRequest* q)
{
    ok(q); /* (the session itself ends when the game closes its handle) */
}

static void file_set_priority(HosRequest* q)
{
    file_of(q)->priority = hos_in(q, 1);
    ok(q);
}

static void file_get_priority(HosRequest* q)
{
    hos_reply(q, 2, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, file_of(q)->priority);
}

static void file_open_link_file(HosRequest* q)
{
    uint32_t h = open_session(*file_of(q));
    hos_reply(q, 1, 2);
    hos_out(q, 1, HOS_OK);
    hos_out_handles(q, 2, 1, &h, 1);
}

static const HosCommandEntry file_commands[] = {
    {0x0802, file_read, "Read"},
    {0x0804, file_get_size, "GetSize"},
    {0x0808, file_close, "Close"},
    {0x080A, file_set_priority, "SetPriority"},
    {0x080B, file_get_priority, "GetPriority"},
    {0x080C, file_open_link_file, "OpenLinkFile"},
    {0, NULL, NULL},
};

const HosService hos_fs_file = {"(fs file)", file_commands};
