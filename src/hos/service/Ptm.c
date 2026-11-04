/* ptm:u: power and the pedometer, as Azahar answers (service/ptm/ptm.cpp): the step history, each hour Azahar's
 * steps_per_hour setting (0 by default). */
#include "../Ipc.h"

#include <string.h>

#include "../Memory.h"

static void get_step_history(HosRequest* q)
{
    uint32_t hours = hos_in(q, 1), desc = hos_in(q, 4), address = hos_in(q, 5);
    memset(hos_pointer(address), 0, 2u * hours);
    hos_reply(q, 1, 2);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, desc);
    hos_out(q, 3, address);
}

static const HosCommandEntry commands[] = {
    {0x000B, get_step_history, "GetStepHistory"},
    {0, NULL, NULL},
};

const HosService hos_ptm_u = {"ptm:u", commands};
