/* nwm::UDS: the local wireless (the Player Search System's neighbours), as Azahar answers it (service/nwm/nwm_uds.cpp)
 * with no network: initialised, not connected, no one around. Its other commands are added as the game calls them;
 * one not here stops the thread and names itself (Ipc.c). */
#include "../Ipc.h"

enum { NOT_CONNECTED = 3 };

static HosObject* status_event; /* (Azahar's connection_status_event: signaled when the status changes; never here) */

static void ok(HosRequest* q)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, HOS_OK);
}

/* InitializeWithVersion: shared memory size, the node's information, the version, the receive buffer's shared
   memory; replies with the connection status event */
static void initialize_with_version(HosRequest* q)
{
    if (!status_event)
        status_event = hos_object_new(HOS_EVENT);
    uint32_t h = hos_handle_new(status_event);
    hos_reply(q, 1, 2);
    hos_out(q, 1, HOS_OK);
    hos_out_handles(q, 2, 0, &h, 1);
}

/* the connection status (0x30 bytes): not connected, everything else zero (Azahar's, after Initialize) */
static void get_connection_status(HosRequest* q)
{
    hos_reply(q, 13, 0);
    hos_out(q, 1, HOS_OK);
    hos_out(q, 2, NOT_CONNECTED);
    for (int k = 3; k <= 13; k++)
        hos_out(q, k, 0);
}

static const HosCommandEntry commands[] = {
    {0x0003, ok, "Shutdown"},
    {0x000B, get_connection_status, "GetConnectionStatus"},
    {0x001B, initialize_with_version, "InitializeWithVersion"},
    {0, NULL, NULL},
};

const HosService hos_nwm_uds = {"nwm::UDS", commands};
