/* The archives fs:USER keeps in a folder on the host (FsHost.c): the game's save data (archive 0x4) and its extra
 * data (0x6), as Azahar keeps them (file_sys/savedata_archive.cpp, archive_extsavedata.cpp, path_parser.cpp). */
#ifndef HOS_FS_HOST_H
#define HOS_FS_HOST_H

#include <stdint.h>
#include <stdio.h>

#include "../Kernel.h"

/* a game path (type 1 empty, 3 ASCII, 4 UTF-16) as "/a/b"; 0 if it is not one (Azahar's InvalidPath) */
int fs_host_path(Cpu* c, uint32_t type, uint32_t size, uint32_t address, char* out, size_t n);

/* the results Azahar gives (file_sys/errors.h) */
enum {
    FS_FILE_NOT_FOUND = 0xC8804470u, FS_PATH_NOT_FOUND = 0xC8804471u, FS_UNEXPECTED_FILE_OR_DIRECTORY = 0xE0C04702u,
    FS_FILE_ALREADY_EXISTS = 0xC82044B4u, FS_DIRECTORY_ALREADY_EXISTS = 0xC82044B9u,
    FS_UNSUPPORTED_OPEN_FLAGS = 0xE0C046F8u, FS_INVALID_PATH = 0xE0E046BEu, FS_NOT_FORMATTED = 0xC8A04554u,
    FS_NOT_FOUND_INVALID_STATE = 0xC8A04478u, FS_WRITE_BEYOND_END = 0xE0E046C1u, FS_INVALID_OPEN_FLAGS = 0xC92044E6u,
    FS_DIRECTORY_NOT_EMPTY = 0xC92044F0u,
};

/* in the archive rooted at root (a host folder): the operations, each returning HOS_OK or a result above */
uint32_t fs_host_open(const char* root, const char* path, uint32_t mode, FILE** out);
uint32_t fs_host_create(const char* root, const char* path, uint64_t size, int allow_zero);
uint32_t fs_host_delete(const char* root, const char* path);
uint32_t fs_host_create_directory(const char* root, const char* path);
uint32_t fs_host_open_directory(const char* root, const char* path, char* out, size_t n);
/* up to count directory entries (Azahar's 0x228-byte Entry) of the host folder dir, from index first, written at to;
   the number written */
uint32_t fs_host_read_directory(const char* dir, uint32_t first, uint32_t count, uint8_t* to);

/* the folder made, with its parents */
int fs_host_make_folders(const char* path);

#endif
