/* hid:USER: the buttons, touch screen and motion sensors, as Azahar gives them (service/hid/hid.cpp): a 0x1000-byte
 * shared memory block the service writes the input into and five one-shot events. The input itself (written as the
 * game runs) is not done yet: the block stays zero, no button pressed. */
#include "../Ipc.h"

static HosObject *memory, *events[5];

HosObject* hos_hid_memory(void)
{
    if (!memory) {
        memory = hos_object_new(HOS_MEMORY_BLOCK);
        memory->block_size = 0x1000;
        for (int i = 0; i < 5; i++)
            events[i] = hos_object_new(HOS_EVENT);
    }
    return memory;
}

static void get_ipc_handles(HosRequest* q)
{
    hos_hid_memory();
    uint32_t h[6] = {hos_handle_new(memory)};
    for (int i = 0; i < 5; i++)
        h[i + 1] = hos_handle_new(events[i]);
    hos_reply(q, 1, 7);
    hos_out(q, 1, HOS_OK);
    hos_out_handles(q, 2, 0, h, 6);
}

static const HosCommandEntry commands[] = {
    {0x000A, get_ipc_handles, "GetIPCHandles"},
    {0, NULL, NULL},
};

const HosService hos_hid_user = {"hid:USER", commands};
