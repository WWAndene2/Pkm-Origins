/* fs:USER and its file sessions, as Azahar answers (service/fs/fs_user.cpp, file.cpp, file_sys/): archives opened by
 * id and path, files in them read through sessions. Done so far: the game's own RomFS (archive 0x3, SelfNCCH, file
 * path type 0: the RomFS's level-3 data from the player's .3ds), and the system's data archives (archive 0x2345678A,
 * NCCH, a title's RomFS by title id: the extracted folder's <title id>.romfs, the whole IVFC image, as Azahar serves
 * its open-source replacements; tools/make_system_archive.py), and the game's save data (archive 0x4: the extracted
 * folder's save/, its format in save.metadata) and extra data (0x6: extdata/<id high>/<id low>/user/, its format in
 * the metadata file beside it), host folders kept as Azahar keeps them (FsHost.c). Any other archive or path prints
 * what the game asked
 * and stops the thread: what it needs is added from that, never guessed. */
#include "../Ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#include "FsHost.h"
#include "../GameImage.h"
#include "../GpuTrace.h"
#include "../Memory.h"

enum { MODULE_FS = 17, SUMMARY_NOT_FOUND = 4, SUMMARY_WRONG_ARGUMENT = 8, LEVEL_STATUS = 25, LEVEL_PERMANENT = 27 };
enum { ARCHIVE_SELF_NCCH = 0x3, ARCHIVE_SAVE_DATA = 0x4, ARCHIVE_EXT_SAVE_DATA = 0x6, ARCHIVE_NCCH = 0x2345678A };

typedef struct {
    int kind; /* 1: the RomFS; 2: data in memory (a system archive's image); 3: a host file (save data, extra data) */
    uint64_t size;
    uint32_t priority;
    uint8_t* data;
    FILE* host;
    uint32_t mode; /* (a host file: read 1, write 2, create 4) */
    int fixed_size; /* (extra data's files: their size never changes; Azahar's FixSizeDiskFile) */
} FsFile;

extern const char* hos_game_dir;

/* a system archive's RomFS image (archive 0x2345678A), by its title id: the extracted folder's <title id>.romfs */
static uint8_t* system_archive(uint64_t title, uint64_t* size)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%016llX.romfs", hos_game_dir, (unsigned long long)title);
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "hos: no %s (tools/make_system_archive.py makes it)\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = malloc(n > 0 ? (size_t)n : 1);
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *size = (uint64_t)n;
    return d;
}

extern const HosService hos_fs_file;
static uint32_t priority;
static uint64_t next_archive = 1;
static struct { uint64_t handle; uint32_t id; char root[512]; } archives[32];

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

static const char* root_of(uint64_t handle);

/* a file in an archive by its path; 0 when this archive / path is not done (the thread then stopped) */
static int open_in(HosRequest* q, uint32_t archive, uint32_t type, uint32_t size, uint32_t address, uint32_t* handle)
{
    if (archive == ARCHIVE_SELF_NCCH && type == 2 && size == 12 && ld32(&q->thread->cpu, address) == 0) {
        FsFile f = {1, hos_romfs_size(), 0, NULL};
        *handle = open_session(f);
        return 1;
    }
    if (archive == ARCHIVE_SELF_NCCH && type == 2 && size == 12 && ld32(&q->thread->cpu, address) == 2) {
        /* an ExeFS file, named by the path's 8 bytes after its type ("icon": the SMDH), Azahar's SelfNCCH */
        char name[9] = {0};
        for (int k = 0; k < 8; k++)
            name[k] = (char)ld8(&q->thread->cpu, address + 4 + k);
        FsFile f = {2, 0, 0, NULL};
        uint32_t n = 0;
        if ((f.data = hos_exefs_file(name, &n))) {
            f.size = n;
            *handle = open_session(f);
            return 1;
        }
    }
    stop(q, "open file", archive, type, size, address);
    return 0;
}

static void open_file_directly(HosRequest* q)
{
    uint32_t archive = hos_in(q, 2), atype = hos_in(q, 3), ftype = hos_in(q, 5), fsize = hos_in(q, 6);
    uint32_t asize, fbytes;
    uint32_t aaddr = hos_in_static(q, 9, &asize), faddr = hos_in_static(q, 11, &fbytes);
    uint32_t h;
    if (archive == ARCHIVE_NCCH && atype == 2 && asize == 16 && ftype == 2 && fsize == 20 &&
        ld32(&q->thread->cpu, faddr + 8) == 0) { /* (a title's RomFS: the file path's type 0) */
        uint64_t title = (uint64_t)ld32(&q->thread->cpu, aaddr + 4) << 32 | ld32(&q->thread->cpu, aaddr);
        FsFile f = {2, 0, 0, NULL};
        if (!(f.data = system_archive(title, &f.size))) {
            stop(q, "open archive (directly)", archive, atype, asize, aaddr);
            return;
        }
        h = open_session(f);
        hos_reply(q, 1, 2);
        hos_out(q, 1, HOS_OK);
        hos_out_handles(q, 2, 1, &h, 1);
        return;
    }
    if (archive != ARCHIVE_SELF_NCCH || atype != 1) {
        stop(q, "open archive (directly)", archive, atype, asize, aaddr);
        return;
    }
    if (!open_in(q, archive, ftype, fsize, faddr, &h))
        return;
    hos_reply(q, 1, 2);
    hos_out(q, 1, HOS_OK);
    hos_out_handles(q, 2, 1, &h, 1);
}

/* a host archive's folder (save data, extra data) and its format file, from its id and path; 0 when not one */
static int host_archive(HosRequest* q, uint32_t id, uint32_t type, uint32_t size, uint32_t address, char* root,
                        char* metadata, size_t n)
{
    Cpu* c = &q->thread->cpu;
    if (id == ARCHIVE_SAVE_DATA && type == 1) {
        snprintf(root, n, "%s/save", hos_game_dir);
        snprintf(metadata, n, "%s/save.metadata", hos_game_dir);
        return 1;
    }
    if (id == ARCHIVE_EXT_SAVE_DATA && type == 2 && size == 12) { /* (media type, save id low, high) */
        snprintf(root, n, "%s/extdata/%08X/%08X/user", hos_game_dir, ld32(c, address + 8), ld32(c, address + 4));
        snprintf(metadata, n, "%s/extdata/%08X/%08X/metadata", hos_game_dir, ld32(c, address + 8),
                 ld32(c, address + 4));
        return 1;
    }
    return 0;
}

static int is_folder(const char* p)
{
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

static void open_archive(HosRequest* q)
{
    uint32_t archive = hos_in(q, 1), type = hos_in(q, 2), size;
    uint32_t address = hos_in_static(q, 4, &size);
    char root[512] = "", metadata[512];
    if (host_archive(q, archive, type, size, address, root, metadata, sizeof root)) {
        if (!is_folder(root)) { /* (Azahar: save data NotFormatted, extra data NotFound) */
            hos_reply(q, 3, 0);
            hos_out(q, 1, archive == ARCHIVE_SAVE_DATA ? FS_NOT_FORMATTED : FS_NOT_FOUND_INVALID_STATE);
            hos_out(q, 2, 0);
            hos_out(q, 3, 0);
            return;
        }
    } else if (archive != ARCHIVE_SELF_NCCH || type != 1) {
        stop(q, "open archive", archive, type, size, address);
        return;
    }
    for (int i = 0; i < 32; i++)
        if (!archives[i].handle) {
            archives[i].handle = next_archive++;
            archives[i].id = archive;
            snprintf(archives[i].root, sizeof archives[i].root, "%s", root);
            hos_reply(q, 3, 0);
            hos_out(q, 1, HOS_OK);
            hos_out(q, 2, (uint32_t)archives[i].handle);
            hos_out(q, 3, (uint32_t)(archives[i].handle >> 32));
            return;
        }
    stop(q, "open archive (33 open)", archive, type, size, address);
}

static uint32_t archive_of(uint64_t handle)
{
    for (int i = 0; i < 32; i++)
        if (archives[i].handle == handle)
            return archives[i].id;
    return 0xFFFFFFFFu;
}

static const char* root_of(uint64_t handle)
{
    for (int i = 0; i < 32; i++)
        if (archives[i].handle == handle)
            return archives[i].root;
    return "";
}

static void reply_handle(HosRequest* q, uint32_t result, uint32_t h)
{
    hos_reply(q, 1, 2);
    hos_out(q, 1, result);
    hos_out_handles(q, 2, 1, &h, 1);
}

#define INVALID_ARCHIVE_HANDLE HOS_RESULT(101, MODULE_FS, SUMMARY_NOT_FOUND, LEVEL_STATUS)

static void open_file(HosRequest* q)
{
    uint64_t handle = (uint64_t)hos_in(q, 3) << 32 | hos_in(q, 2);
    uint32_t type = hos_in(q, 4), size = hos_in(q, 5), mode = hos_in(q, 6), bytes;
    uint32_t address = hos_in_static(q, 8, &bytes);
    uint32_t archive = archive_of(handle);
    if (archive == 0xFFFFFFFFu) {
        reply_handle(q, INVALID_ARCHIVE_HANDLE, 0);
        return;
    }
    uint32_t h;
    if (archive == ARCHIVE_SAVE_DATA || archive == ARCHIVE_EXT_SAVE_DATA) {
        char path[512];
        if (!fs_host_path(&q->thread->cpu, type, size, address, path, sizeof path)) {
            reply_handle(q, FS_INVALID_PATH, 0);
            return;
        }
        FsFile f = {3, 0, 0, NULL, NULL, mode, archive == ARCHIVE_EXT_SAVE_DATA};
        uint32_t r = fs_host_open(root_of(handle), path, mode, &f.host);
        reply_handle(q, r, r == HOS_OK ? open_session(f) : 0);
        return;
    }
    if (!open_in(q, archive, type, size, address, &h))
        return;
    reply_handle(q, HOS_OK, h);
}

/* the host archives' other commands: each answers with Azahar's result */
static int host_path_of(HosRequest* q, uint64_t handle, uint32_t type, uint32_t size, uint32_t address, char* path,
                        uint32_t* result)
{
    uint32_t archive = archive_of(handle);
    if (archive != ARCHIVE_SAVE_DATA && archive != ARCHIVE_EXT_SAVE_DATA) {
        *result = archive == 0xFFFFFFFFu ? INVALID_ARCHIVE_HANDLE : 0;
        return 0;
    }
    if (!fs_host_path(&q->thread->cpu, type, size, address, path, 512)) {
        *result = FS_INVALID_PATH;
        return 0;
    }
    return 1;
}

static void result_only(HosRequest* q, uint32_t r)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, r);
}

static void create_file(HosRequest* q)
{
    uint64_t handle = (uint64_t)hos_in(q, 3) << 32 | hos_in(q, 2);
    uint32_t type = hos_in(q, 4), size = hos_in(q, 5), bytes, r;
    uint64_t file_size = (uint64_t)hos_in(q, 8) << 32 | hos_in(q, 7);
    uint32_t address = hos_in_static(q, 9, &bytes);
    char path[512];
    if (!host_path_of(q, handle, type, size, address, path, &r)) {
        if (r)
            result_only(q, r);
        else
            stop(q, "create file", archive_of(handle), type, size, address);
        return;
    }
    result_only(q, fs_host_create(root_of(handle), path, file_size, archive_of(handle) == ARCHIVE_SAVE_DATA));
}

static void delete_file(HosRequest* q)
{
    uint64_t handle = (uint64_t)hos_in(q, 3) << 32 | hos_in(q, 2);
    uint32_t type = hos_in(q, 4), size = hos_in(q, 5), bytes, r;
    uint32_t address = hos_in_static(q, 6, &bytes);
    char path[512];
    if (!host_path_of(q, handle, type, size, address, path, &r)) {
        if (r)
            result_only(q, r);
        else
            stop(q, "delete file", archive_of(handle), type, size, address);
        return;
    }
    result_only(q, fs_host_delete(root_of(handle), path));
}

static void create_directory(HosRequest* q)
{
    uint64_t handle = (uint64_t)hos_in(q, 3) << 32 | hos_in(q, 2);
    uint32_t type = hos_in(q, 4), size = hos_in(q, 5), bytes, r;
    uint32_t address = hos_in_static(q, 7, &bytes);
    char path[512];
    if (!host_path_of(q, handle, type, size, address, path, &r)) {
        if (r)
            result_only(q, r);
        else
            stop(q, "create directory", archive_of(handle), type, size, address);
        return;
    }
    result_only(q, fs_host_create_directory(root_of(handle), path));
}

typedef struct {
    char folder[1024];
    uint32_t next;
} FsDirectory;

extern const HosService hos_fs_directory;

static void open_directory(HosRequest* q)
{
    uint64_t handle = (uint64_t)hos_in(q, 2) << 32 | hos_in(q, 1);
    uint32_t type = hos_in(q, 3), size = hos_in(q, 4), bytes, r;
    uint32_t address = hos_in_static(q, 5, &bytes);
    char path[512];
    if (!host_path_of(q, handle, type, size, address, path, &r)) {
        if (r)
            reply_handle(q, r, 0);
        else
            stop(q, "open directory", archive_of(handle), type, size, address);
        return;
    }
    FsDirectory* d = calloc(1, sizeof *d);
    r = fs_host_open_directory(root_of(handle), path, d->folder, sizeof d->folder);
    if (r != HOS_OK) {
        free(d);
        reply_handle(q, r, 0);
        return;
    }
    uint32_t h = hos_session_open(&hos_fs_directory);
    hos_handle_get(h, NULL)->service_data = d;
    reply_handle(q, HOS_OK, h);
}

static void control_archive(HosRequest* q)
{
    result_only(q, HOS_OK); /* (Azahar: every archive's Control answers success; the commit of save data included) */
}

/* the format of save data or extra data: total size, directories, files, duplicate data (16 bytes, Azahar's
   ArchiveFormatInfo) */
static void get_format_info(HosRequest* q)
{
    uint32_t archive = hos_in(q, 1), type = hos_in(q, 2), size;
    uint32_t address = hos_in_static(q, 4, &size);
    char root[512], metadata[512];
    if (!host_archive(q, archive, type, size, address, root, metadata, sizeof root)) {
        stop(q, "get format info", archive, type, size, address);
        return;
    }
    uint32_t info[4] = {0};
    FILE* f = fopen(metadata, "rb");
    hos_reply(q, 5, 0);
    if (!f || fread(info, 1, 16, f) != 16) {
        hos_out(q, 1, FS_NOT_FORMATTED);
        for (int k = 2; k <= 5; k++)
            hos_out(q, k, 0);
    } else {
        hos_out(q, 1, HOS_OK);
        hos_out(q, 2, info[0]);
        hos_out(q, 3, info[1]);
        hos_out(q, 4, info[2]);
        hos_out(q, 5, info[3] & 0xFF);
    }
    if (f)
        fclose(f);
}

static int remove_one(const char* p, const struct stat* st, int kind, struct FTW* ftw)
{
    (void)st;
    (void)kind;
    (void)ftw;
    return remove(p);
}

static void remove_tree(const char* p)
{
    nftw(p, remove_one, 16, FTW_DEPTH | FTW_PHYS); /* (Azahar's DeleteDirRecursively) */
}

static void format_save_data(HosRequest* q)
{
    uint32_t archive = hos_in(q, 1), type = hos_in(q, 2), size;
    uint32_t info[4] = {hos_in(q, 4) * 512, hos_in(q, 5), hos_in(q, 6), hos_in(q, 9) & 0xFF};
    uint32_t address = hos_in_static(q, 10, &size);
    char root[512], metadata[512];
    if (archive != ARCHIVE_SAVE_DATA) {
        result_only(q, FS_INVALID_PATH);
        return;
    }
    if (type != 1) {
        result_only(q, HOS_RESULT(1012, MODULE_FS, 6, LEVEL_PERMANENT)); /* (Azahar: UnimplementedFunction) */
        return;
    }
    host_archive(q, archive, type, size, address, root, metadata, sizeof root);
    remove_tree(root);
    FILE* f = NULL;
    if (fs_host_make_folders(root) || !(f = fopen(metadata, "wb"))) {
        stop(q, "format save data (the host folder cannot be made)", archive, type, size, address);
        return;
    }
    fwrite(info, 1, 16, f);
    fclose(f);
    result_only(q, HOS_OK);
}

static void create_ext_save_data(HosRequest* q)
{
    uint32_t low = hos_in(q, 2), high = hos_in(q, 3);
    uint32_t info[4] = {hos_in(q, 7), hos_in(q, 5), hos_in(q, 6), 0}; /* (total size: the size limit's low word) */
    uint32_t icon_size = hos_in(q, 9), desc = hos_in(q, 10), icon = hos_in(q, 11);
    char base[512], path[600];
    snprintf(base, sizeof base, "%s/extdata/%08X/%08X", hos_game_dir, high, low);
    snprintf(path, sizeof path, "%s/user", base);
    int bad = fs_host_make_folders(path);
    snprintf(path, sizeof path, "%s/boss", base);
    bad = bad || fs_host_make_folders(path);
    snprintf(path, sizeof path, "%s/metadata", base);
    FILE* f = bad ? NULL : fopen(path, "wb");
    if (f) {
        fwrite(info, 1, 16, f);
        fclose(f);
        snprintf(path, sizeof path, "%s/icon", base);
        if (icon_size && (f = fopen(path, "wb"))) {
            fwrite(hos_pointer(icon), 1, icon_size, f);
            fclose(f);
        }
    }
    hos_reply(q, 1, 2);
    hos_out(q, 1, f ? HOS_OK : 0xFFFFFFFFu /* (Azahar: ResultUnknown) */);
    hos_out(q, 2, desc);
    hos_out(q, 3, icon);
}

static void get_this_save_data_secure_value(HosRequest* q)
{
    hos_reply(q, 5, 0); /* (Azahar's default: no secure value, the process a game card) */
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, 0);
    hos_out(q, 3, 1);
    hos_out(q, 4, 0);
    hos_out(q, 5, 0);
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
    {0x0804, delete_file, "DeleteFile"},
    {0x0808, create_file, "CreateFile"},
    {0x0809, create_directory, "CreateDirectory"},
    {0x080B, open_directory, "OpenDirectory"},
    {0x080C, open_archive, "OpenArchive"},
    {0x080D, control_archive, "ControlArchive"},
    {0x080E, close_archive, "CloseArchive"},
    {0x0845, get_format_info, "GetFormatInfo"},
    {0x084C, format_save_data, "FormatSaveData"},
    {0x0851, create_ext_save_data, "CreateExtSaveData"},
    {0x0861, ok, "InitializeWithSdkVersion"},
    {0x086F, get_this_save_data_secure_value, "GetThisSaveDataSecureValue"},
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
    size_t n = 0;
    if (f->kind == 1) {
        n = hos_romfs_read(offset, hos_pointer(address), length);
        hos_gpu_trace_file(address, (uint32_t)n, offset);
    } else if (f->kind == 3) {
        if (!(f->mode & 1)) { /* (Azahar's DiskFile: no read flag) */
            hos_reply(q, 2, 2);
            hos_out(q, 1, 0xE0E046BCu /* InvalidReadFlag */);
            hos_out(q, 2, 0);
            hos_out(q, 3, desc);
            hos_out(q, 4, address);
            return;
        }
        fseeko(f->host, (off_t)offset, SEEK_SET);
        n = fread(hos_pointer(address), 1, length, f->host);
    } else if (f->kind == 2 && offset < f->size) {
        n = f->size - offset < length ? (size_t)(f->size - offset) : length;
        memcpy(hos_pointer(address), f->data + offset, n);
    }
    hos_reply(q, 2, 2);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, (uint32_t)n);
    hos_out(q, 3, desc);
    hos_out(q, 4, address);
}

static uint64_t host_size(FsFile* f)
{
    struct stat st;
    return fstat(fileno(f->host), &st) ? 0 : (uint64_t)st.st_size;
}

static void file_write(HosRequest* q)
{
    FsFile* f = file_of(q);
    uint64_t offset = (uint64_t)hos_in(q, 2) << 32 | hos_in(q, 1);
    uint32_t length = hos_in(q, 3), flush = hos_in(q, 4), desc = hos_in(q, 5), address = hos_in(q, 6);
    uint32_t r = HOS_OK;
    size_t n = 0;
    if (f->kind != 3 || !(f->mode & 2)) {
        r = FS_INVALID_OPEN_FLAGS;
    } else {
        uint64_t size = host_size(f);
        if (f->fixed_size && offset > size)
            r = FS_WRITE_BEYOND_END;
        else {
            if (f->fixed_size && offset + length > size)
                length = (uint32_t)(size - offset);
            fseeko(f->host, (off_t)offset, SEEK_SET);
            n = fwrite(hos_pointer(address), 1, length, f->host);
            if (flush)
                fflush(f->host);
        }
    }
    hos_reply(q, 2, 2);
    hos_out(q, 1, r);
    hos_out(q, 2, (uint32_t)n);
    hos_out(q, 3, desc);
    hos_out(q, 4, address);
}

static void file_set_size(HosRequest* q)
{
    FsFile* f = file_of(q);
    uint64_t size = (uint64_t)hos_in(q, 2) << 32 | hos_in(q, 1);
    if (f->kind != 3 || !(f->mode & 2)) {
        result_only(q, FS_INVALID_OPEN_FLAGS);
        return;
    }
    if (f->fixed_size) { /* (Azahar's FixSizeDiskFile::SetSize: refused, and the service answers so) */
        stop(q, "set size of an extra data file", 6, 0, 0, 0);
        return;
    }
    fflush(f->host);
    result_only(q, ftruncate(fileno(f->host), (off_t)size) ? 0xFFFFFFFFu : HOS_OK);
}

static void file_flush(HosRequest* q)
{
    FsFile* f = file_of(q);
    if (f->kind == 3)
        fflush(f->host);
    ok(q);
}

static void file_get_size(HosRequest* q)
{
    FsFile* f = file_of(q);
    if (f->kind == 3)
        f->size = host_size(f);
    hos_reply(q, 3, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, (uint32_t)f->size);
    hos_out(q, 3, (uint32_t)(f->size >> 32));
}

static void file_close(HosRequest* q)
{
    FsFile* f = file_of(q);
    if (f->kind == 3 && f->host) {
        fclose(f->host);
        f->host = NULL;
    }
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
    {0x0803, file_write, "Write"},
    {0x0804, file_get_size, "GetSize"},
    {0x0805, file_set_size, "SetSize"},
    {0x0809, file_flush, "Flush"},
    {0x0808, file_close, "Close"},
    {0x080A, file_set_priority, "SetPriority"},
    {0x080B, file_get_priority, "GetPriority"},
    {0x080C, file_open_link_file, "OpenLinkFile"},
    {0, NULL, NULL},
};

const HosService hos_fs_file = {"(fs file)", file_commands};

/* the directory sessions (directory.cpp) */
static void directory_read(HosRequest* q)
{
    FsDirectory* d = q->session->service_data;
    uint32_t count = hos_in(q, 1), desc = hos_in(q, 2), address = hos_in(q, 3);
    uint32_t n = fs_host_read_directory(d->folder, d->next, count, hos_pointer(address));
    d->next += n;
    hos_reply(q, 2, 2);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, n);
    hos_out(q, 3, desc);
    hos_out(q, 4, address);
}

static const HosCommandEntry directory_commands[] = {
    {0x0801, directory_read, "Read"},
    {0x0802, ok, "Close"},
    {0, NULL, NULL},
};

const HosService hos_fs_directory = {"(fs directory)", directory_commands};
