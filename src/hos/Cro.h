/* The CRO module loader (Cro.c): ldr:ro's LoadCRS and LoadCRO on the game's memory. */
#ifndef HOS_CRO_H
#define HOS_CRO_H

#include <stdint.h>

/* the static module (static.crs), mapped at crs: rebased there; 0 on success */
int hos_cro_load_crs(uint32_t crs, uint32_t size);

/* a module mapped at m (its file, size bytes): rebased, linked with the loaded ones (the list crs heads), registered
   and fixed at fix_level; its size once fixed, 0 on failure (LoadCRO: the newer command passes link_on_load_bug_fix) */
uint32_t hos_cro_load(uint32_t m, uint32_t size, uint32_t data, uint32_t data_size, uint32_t bss, uint32_t bss_size,
                      int auto_link, uint32_t fix_level, uint32_t crs, int link_on_load_bug_fix);

/* the address of a named export of the loaded module m (0 if none) */
uint32_t hos_cro_find_export(uint32_t m, const char* name);

#endif
