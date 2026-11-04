/* ndm:u: the network daemon manager, as Azahar keeps it (service/ndm/ndm_u.cpp): its daemons' suspend counts (a
 * resume of a daemon not suspended is its error); the rest acknowledged. */
#include "../Ipc.h"

enum { MODULE_NDM = 26, SUMMARY_INVALID_STATE = 5, LEVEL_STATUS = 25 };

static int suspended[4];

static void ok(HosRequest* q)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, HOS_OK);
}

static void suspend_daemons(HosRequest* q)
{
    uint32_t mask = hos_in(q, 1) & 0xF;
    for (int i = 0; i < 4; i++)
        if (mask & (1u << i))
            suspended[i]++;
    ok(q);
}

static void resume_daemons(HosRequest* q)
{
    uint32_t mask = hos_in(q, 1) & 0xF;
    for (int i = 0; i < 4; i++)
        if (mask & (1u << i) && suspended[i] == 0) {
            hos_reply(q, 1, 0);
            hos_out(q, 1, HOS_RESULT(13, MODULE_NDM, SUMMARY_INVALID_STATE, LEVEL_STATUS));
            return;
        }
    for (int i = 0; i < 4; i++)
        if (mask & (1u << i))
            suspended[i]--;
    ok(q);
}

static const HosCommandEntry commands[] = {
    {0x0001, ok, "EnterExclusiveState"},
    {0x0002, ok, "LeaveExclusiveState"},
    {0x0006, suspend_daemons, "SuspendDaemons"},
    {0x0007, resume_daemons, "ResumeDaemons"},
    {0x0014, ok, "OverrideDefaultDaemons"},
    {0, NULL, NULL},
};

const HosService hos_ndm_u = {"ndm:u", commands};
