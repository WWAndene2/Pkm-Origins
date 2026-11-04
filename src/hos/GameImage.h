/* The player's copy of the game (a decrypted .3ds), read where the 3DS reads it: its first partition's NCCH, the
 * ExeFS's files (the icon: its SMDH) and the RomFS's level-3 data (Azahar's file_sys/ncch_container.cpp: from the
 * RomFS's offset + 0x1000). */
#ifndef HOS_GAME_IMAGE_H
#define HOS_GAME_IMAGE_H

#include <stddef.h>
#include <stdint.h>

int hos_game_open(const char* path); /* 0 on success */
/* reads from the RomFS's level-3 data (offset from its start); the bytes read */
size_t hos_romfs_read(uint64_t offset, void* out, size_t size);
uint64_t hos_romfs_size(void);
/* an ExeFS file ("icon"...) in a new buffer, its size in *size; NULL if absent */
uint8_t* hos_exefs_file(const char* name, uint32_t* size);
/* the regions the game's icon allows, in order (0 Japan, 1 Americas, 2 Europe, 3 Australia, 4 China, 5 Korea,
   6 Taiwan); how many */
int hos_game_regions(uint32_t* out);

#endif
