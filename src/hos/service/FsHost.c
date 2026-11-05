/* The host-folder archives of fs:USER: see FsHost.h. The status of a path in the folder is Azahar's PathParser's: its
 * mount point missing, a folder of it missing, a file where a folder should be, a folder, nothing, a file. */
#include "FsHost.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { INVALID_MOUNT_POINT, PATH_NOT_FOUND, FILE_IN_PATH, DIRECTORY_FOUND, NOT_FOUND, FILE_FOUND };

int fs_host_path(Cpu* c, uint32_t type, uint32_t size, uint32_t address, char* out, size_t n)
{
    size_t k = 0;
    if (type == 1) {
        out[0] = 0;
        return 1;
    }
    for (uint32_t i = 0; i < size; i += type == 4 ? 2 : 1) {
        uint32_t ch = type == 4 ? ld16(c, address + i) : ld8(c, address + i);
        if (!ch)
            break;
        if (ch >= 0x80 || ch == '\\' || k + 1 >= n)
            return 0; /* (names outside ASCII: none in the two games' saves) */
        out[k++] = (char)ch;
    }
    out[k] = 0;
    if (type != 3 && type != 4)
        return 0;
    if (out[0] != '/')
        return 0;
    for (char* p = out; (p = strstr(p, "/..")); p++)
        if (p[3] == '/' || !p[3])
            return 0;
    return 1;
}

static int is_dir(const char* p)
{
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
}

static int exists(const char* p)
{
    struct stat st;
    return !stat(p, &st);
}

static int status(const char* root, const char* path, char* full, size_t n)
{
    if (!is_dir(root))
        return INVALID_MOUNT_POINT;
    snprintf(full, n, "%s%s", root, path);
    if (!path[0] || !strcmp(path, "/"))
        return DIRECTORY_FOUND;
    char part[1024];
    snprintf(part, sizeof part, "%s", full);
    size_t base = strlen(root);
    for (char* s = part + base + 1; (s = strchr(s, '/')); s++) {
        *s = 0;
        if (!exists(part))
            return PATH_NOT_FOUND;
        if (!is_dir(part))
            return FILE_IN_PATH;
        *s = '/';
    }
    if (!exists(full))
        return NOT_FOUND;
    return is_dir(full) ? DIRECTORY_FOUND : FILE_FOUND;
}

uint32_t fs_host_open(const char* root, const char* path, uint32_t mode, FILE** out)
{
    char full[1024];
    if (!mode || ((mode & 4) && !(mode & 2)))
        return FS_UNSUPPORTED_OPEN_FLAGS;
    switch (status(root, path, full, sizeof full)) {
    case INVALID_MOUNT_POINT:
        return FS_FILE_NOT_FOUND;
    case PATH_NOT_FOUND:
        return FS_PATH_NOT_FOUND;
    case FILE_IN_PATH:
    case DIRECTORY_FOUND:
        return FS_UNEXPECTED_FILE_OR_DIRECTORY;
    case NOT_FOUND:
        if (!(mode & 4))
            return FS_FILE_NOT_FOUND;
        fclose(fopen(full, "wb"));
        break;
    }
    *out = fopen(full, mode & 2 ? "r+b" : "rb");
    return *out ? HOS_OK : FS_FILE_NOT_FOUND;
}

uint32_t fs_host_create(const char* root, const char* path, uint64_t size, int allow_zero)
{
    char full[1024];
    switch (status(root, path, full, sizeof full)) {
    case INVALID_MOUNT_POINT:
        return FS_FILE_NOT_FOUND;
    case PATH_NOT_FOUND:
        return FS_PATH_NOT_FOUND;
    case FILE_IN_PATH:
        return FS_UNEXPECTED_FILE_OR_DIRECTORY;
    case DIRECTORY_FOUND:
    case FILE_FOUND:
        return FS_FILE_ALREADY_EXISTS;
    }
    if (!size && !allow_zero)
        return FS_UNSUPPORTED_OPEN_FLAGS;
    FILE* f = fopen(full, "wb");
    if (!f)
        return FS_FILE_NOT_FOUND;
    int bad = size && ftruncate(fileno(f), (off_t)size);
    fclose(f);
    return bad ? 0x086047E9u /* (Azahar: TooLarge, OutOfResource, Info) */ : HOS_OK;
}

uint32_t fs_host_delete(const char* root, const char* path)
{
    char full[1024];
    switch (status(root, path, full, sizeof full)) {
    case PATH_NOT_FOUND:
        return FS_PATH_NOT_FOUND;
    case FILE_FOUND:
        return unlink(full) ? FS_FILE_NOT_FOUND : HOS_OK;
    default:
        return FS_FILE_NOT_FOUND;
    }
}

uint32_t fs_host_create_directory(const char* root, const char* path)
{
    char full[1024];
    switch (status(root, path, full, sizeof full)) {
    case INVALID_MOUNT_POINT:
        return FS_FILE_NOT_FOUND;
    case PATH_NOT_FOUND:
        return FS_PATH_NOT_FOUND;
    case FILE_IN_PATH:
        return FS_UNEXPECTED_FILE_OR_DIRECTORY;
    case DIRECTORY_FOUND:
    case FILE_FOUND:
        return FS_DIRECTORY_ALREADY_EXISTS;
    }
    return mkdir(full, 0755) ? 0xC92047EFu /* (Azahar: NoData, Canceled, Status) */ : HOS_OK;
}

uint32_t fs_host_open_directory(const char* root, const char* path, char* out, size_t n)
{
    switch (status(root, path, out, n)) {
    case INVALID_MOUNT_POINT:
        return FS_FILE_NOT_FOUND;
    case PATH_NOT_FOUND:
        return FS_PATH_NOT_FOUND;
    case FILE_IN_PATH:
    case FILE_FOUND:
    case NOT_FOUND:
        return FS_UNEXPECTED_FILE_OR_DIRECTORY;
    }
    return HOS_OK;
}

static int by_name(const void* a, const void* b)
{
    return strcmp(*(char* const*)a, *(char* const*)b);
}

uint32_t fs_host_read_directory(const char* dir, uint32_t first, uint32_t count, uint8_t* to)
{
    DIR* d = opendir(dir);
    if (!d)
        return 0;
    char* names[1024];
    int n = 0;
    for (struct dirent* e; (e = readdir(d)) && n < 1024;)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            names[n++] = strdup(e->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, by_name); /* (Azahar's FSTEntry order: the host's; sorted here) */
    uint32_t written = 0;
    for (int i = (int)first; i < n && written < count; i++, written++) {
        uint8_t* e = to + 0x228 * written;
        memset(e, 0, 0x228);
        for (size_t k = 0; names[i][k] && k < 0x105; k++)
            e[2 * k] = (uint8_t)names[i][k];
        /* the 8.3 name: the name's first 8 and extension's 3 characters, upper case (Azahar's SplitFilename83) */
        const char* dot = strrchr(names[i], '.');
        size_t base = dot ? (size_t)(dot - names[i]) : strlen(names[i]);
        for (size_t k = 0; k < 8 && k < base; k++)
            e[0x20C + k] = (uint8_t)(names[i][k] >= 'a' && names[i][k] <= 'z' ? names[i][k] - 32 : names[i][k]);
        for (size_t k = 0; k < 3; k++)
            e[0x216 + k] = dot && dot[1 + k] ? (uint8_t)(dot[1 + k] >= 'a' && dot[1 + k] <= 'z' ? dot[1 + k] - 32
                                                                                                 : dot[1 + k])
                                             : ' ';
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, names[i]);
        struct stat st;
        stat(full, &st);
        e[0x21C] = S_ISDIR(st.st_mode);
        e[0x21D] = names[i][0] == '.';
        e[0x21E] = !S_ISDIR(st.st_mode);
        uint64_t size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
        memcpy(e + 0x220, &size, 8);
    }
    for (int i = 0; i < n; i++)
        free(names[i]);
    return written;
}

int fs_host_make_folders(const char* path)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char* s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (mkdir(p, 0755) && errno != EEXIST)
                return -1;
            *s = '/';
        }
    return mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
}
