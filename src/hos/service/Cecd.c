/* cecd:u: StreetPass (the CEC daemon), as Azahar answers it (service/cecd/cecd.cpp). Its commands are added as the game
 * calls them; one not here stops the thread and names itself (Ipc.c). */
#include "../Ipc.h"

static const HosCommandEntry commands[] = {
    {0, NULL, NULL},
};

const HosService hos_cecd_u = {"cecd:u", commands};
