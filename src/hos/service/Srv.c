/* srv: the service manager (Azahar's service/sm/srv.cpp): the game registers, gets the notification semaphore,
 * subscribes to notifications and gets a session to each service by name. */
#include "../Ipc.h"

#include <stdio.h>
#include <string.h>

static void register_client(HosRequest* q)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, HOS_OK);
}

static void enable_notification(HosRequest* q)
{
    static HosObject* semaphore; /* (srv's notification semaphore: one per process) */
    if (!semaphore) {
        semaphore = hos_object_new(HOS_SEMAPHORE);
        semaphore->max_count = 0x10;
    }
    uint32_t h = hos_handle_new(semaphore);
    hos_reply(q, 1, 2);
    hos_out(q, 1, HOS_OK);
    hos_out_handles(q, 2, 0, &h, 1);
}

static void get_service_handle(HosRequest* q)
{
    char name[9] = {0};
    uint32_t w0 = hos_in(q, 1), w1 = hos_in(q, 2), len = hos_in(q, 3);
    memcpy(name, &w0, 4);
    memcpy(name + 4, &w1, 4);
    if (len > 8) {
        hos_reply(q, 1, 0);
        hos_out(q, 1, 0xD9006405); /* (Azahar's ResultInvalidNameSize) */
        return;
    }
    name[len] = 0;
    const HosService* s = hos_service_find(name);
    if (!s) {
        fprintf(stderr, "hos: the game asks for the service %s, not done\n", name);
        q->thread->cpu.fault = HOS_FAULT_SERVICE;
        return;
    }
    uint32_t h = hos_session_open(s);
    hos_reply(q, 1, 2);
    hos_out(q, 1, HOS_OK);
    hos_out_handles(q, 2, 1, &h, 1);
}

static void subscribe(HosRequest* q)
{
    hos_reply(q, 1, 0);
    hos_out(q, 1, HOS_OK);
}

static const HosCommandEntry commands[] = {
    {0x0001, register_client, "RegisterClient"},
    {0x0002, enable_notification, "EnableNotification"},
    {0x0005, get_service_handle, "GetServiceHandle"},
    {0x0009, subscribe, "Subscribe"},
    {0, NULL, NULL},
};

const HosService hos_srv = {"srv:", commands};
