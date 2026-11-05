/* The CRO module loader (Cro.c): ldr:ro's LoadCRS and LoadCRO on the game's memory. */
#ifndef HOS_CRO_H
#define HOS_CRO_H

#include <stddef.h>
#include <stdint.h>

/* the static module (static.crs), mapped at crs: rebased there; 0 on success */
int hos_cro_load_crs(uint32_t crs, uint32_t size);

/* a module mapped at m (its file, size bytes): rebased, linked with the loaded ones (the list crs heads), registered
   and fixed at fix_level; its size once fixed, 0 on failure (LoadCRO: the newer command passes link_on_load_bug_fix) */
uint32_t hos_cro_load(uint32_t m, uint32_t size, uint32_t data, uint32_t data_size, uint32_t bss, uint32_t bss_size,
                      int auto_link, uint32_t fix_level, uint32_t crs, int link_on_load_bug_fix);

/* the address of a named export of the loaded module m (0 if none) */
uint32_t hos_cro_find_export(uint32_t m, const char* name);

/* the loaded module m unloaded (unregistered, unlinked, its relocations cleared if not fixed, unrebased); 0 on success */
int hos_cro_unload(uint32_t m, uint32_t crs);

/* the loaded module m's fixed size and its name */
uint32_t hos_cro_fixed_size(uint32_t m);
void hos_cro_name(uint32_t m, char* out, size_t n);

/* the translated modules' bases (tools/build_game.py --modules: table.c): a module named name loaded at address (1 if
   it was translated), the one at address unloaded */
int hos_module_loaded(const char* name, uint32_t address);
void hos_module_unloaded(uint32_t address);

/* a loaded module's place as "<module>+<offset>" (service/Ldr.c); 0 if address is in none */
int hos_ldr_place(uint32_t address, char* out, size_t n);

#endif
