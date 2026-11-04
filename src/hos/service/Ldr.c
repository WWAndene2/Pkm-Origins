/* ldr:ro: the CRO module loader's service, as Azahar's (service/ldr_ro/ldr_ro.cpp), its work done by Cro.c. The 3DS
 * maps a module's file (which the game read into its heap) at the address the game chose, the heap's pages then
 * locked; here the file is copied there instead, and copied back when the module unloads (and, past its fixed size,
 * as soon as it is loaded: those pages go back to the game), which leaves the game's memory as the mapping would. A
 * module loaded is announced to the translated code (hos_module_loaded: its functions found from then on).
 */
#include "../Ipc.h"

#include <stdio.h>
#include <string.h>

#include "../Cro.h"
#include "../Memory.h"

enum { MODULE_RO = 31 };

static uint32_t crs; /* the static module's address, 0 before Initialize */

static void ok(HosRequest* q)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, HOS_OK);
}

static void stop(HosRequest* q, const char* what)
{
    fprintf(stderr, "hos: ldr:ro %s not done\n", what);
    q->thread->cpu.fault = HOS_FAULT_SERVICE;
}

static void initialize(HosRequest* q)
{
    uint32_t buffer = hos_in(q, 1), size = hos_in(q, 2), address = hos_in(q, 3);
    if (crs || hos_memory_map(address, size)) {
        stop(q, crs ? "Initialize twice" : "Initialize (its address cannot be mapped)");
        return;
    }
    memcpy(hos_pointer(address), hos_pointer(buffer), size);
    if (hos_cro_load_crs(address, size)) {
        stop(q, "Initialize (the static module does not rebase)");
        return;
    }
    crs = address;
    ok(q);
}

static void load_cro(HosRequest* q, int bug_fix)
{
    uint32_t buffer = hos_in(q, 1), address = hos_in(q, 2), size = hos_in(q, 3), data = hos_in(q, 4);
    uint32_t data_size = hos_in(q, 6), bss = hos_in(q, 7), bss_size = hos_in(q, 8);
    int auto_link = hos_in(q, 9) & 0xFF;
    uint32_t fix_level = hos_in(q, 10);
    if (!crs || hos_memory_map(address, size)) {
        stop(q, !crs ? "LoadCRO before Initialize" : "LoadCRO (its address cannot be mapped)");
        return;
    }
    memcpy(hos_pointer(address), hos_pointer(buffer), size);
    uint32_t fixed = hos_cro_load(address, size, data, data_size, bss, bss_size, auto_link, fix_level, crs, bug_fix);
    if (!fixed) {
        stop(q, "LoadCRO (the module does not load)");
        return;
    }
    if (fixed < size) { /* (past the fixed size: the game's pages again) */
        memcpy(hos_pointer(buffer + fixed), hos_pointer(address + fixed), size - fixed);
        hos_memory_unmap(address + fixed, size - fixed);
    }
    char name[64];
    hos_cro_name(address, name, sizeof name);
    if (!hos_module_loaded(name, address))
        fprintf(stderr, "hos: module %s loaded at %08X: not translated (its functions stop the game when called)\n",
                name, address);
    hos_reply(q, 2, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, fixed);
}

static void load_cro_old(HosRequest* q)
{
    load_cro(q, 0);
}

static void load_cro_new(HosRequest* q)
{
    load_cro(q, 1);
}

static void unload_cro(HosRequest* q)
{
    uint32_t address = hos_in(q, 1), buffer = hos_in(q, 3);
    uint32_t fixed = hos_cro_fixed_size(address);
    if (!crs || hos_cro_unload(address, crs)) {
        stop(q, "UnloadCRO (the module does not unload)");
        return;
    }
    hos_module_unloaded(address);
    memcpy(hos_pointer(buffer), hos_pointer(address), fixed);
    hos_memory_unmap(address, fixed);
    ok(q);
}

static void link_cro(HosRequest* q)
{
    stop(q, "LinkCRO");
}

static void unlink_cro(HosRequest* q)
{
    stop(q, "UnlinkCRO");
}

static void shutdown_ro(HosRequest* q)
{
    stop(q, "Shutdown");
}

static const HosCommandEntry commands[] = {
    {0x0001, initialize, "Initialize"},
    {0x0002, ok, "LoadCRR"},
    {0x0003, ok, "UnloadCRR"},
    {0x0004, load_cro_old, "LoadCRO"},
    {0x0005, unload_cro, "UnloadCRO"},
    {0x0006, link_cro, "LinkCRO"},
    {0x0007, unlink_cro, "UnlinkCRO"},
    {0x0008, shutdown_ro, "Shutdown"},
    {0x0009, load_cro_new, "LoadCRO_New"},
    {0, NULL, NULL},
};

const HosService hos_ldr_ro = {"ldr:ro", commands};
