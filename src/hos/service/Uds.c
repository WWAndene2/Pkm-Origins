/* nwm::UDS: the local wireless (the Player Search System's neighbours), as Azahar answers it (service/nwm/nwm_uds.cpp).
 * Its commands are added as the game calls them; one not here stops the thread and names itself (Ipc.c). */
#include "../Ipc.h"

static const HosCommandEntry commands[] = {
    {0, NULL, NULL},
};

const HosService hos_nwm_uds = {"nwm::UDS", commands};
