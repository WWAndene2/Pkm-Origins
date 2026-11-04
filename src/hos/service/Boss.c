/* boss:U: SpotPass, as Azahar answers on a console that never used it (service/boss/boss.cpp, online_service.cpp:
 * no database, InitializeSession succeeds; the storage calls acknowledged, its entry info zero). */
#include "../Ipc.h"

static void ok(HosRequest* q)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, HOS_OK);
}

static void get_storage_entry_info(HosRequest* q)
{
    hos_reply(q, 3, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, 0);
    hos_out(q, 3, 0);
}

static const HosCommandEntry commands[] = {
    {0x0001, ok, "InitializeSession"},
    {0x0003, ok, "UnregisterStorage"},
    {0x002F, ok, "RegisterStorageEntry"},
    {0x0030, get_storage_entry_info, "GetStorageEntryInfo"},
    {0, NULL, NULL},
};

const HosService hos_boss_u = {"boss:U", commands};
