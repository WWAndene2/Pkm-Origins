/* SendSyncRequest to a service, sessions and ports (Ipc.h). */
#include "Ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const HosService* const services[] = {&hos_srv, &hos_apt_a, &hos_apt_u, &hos_ndm_u, &hos_cfg_u, &hos_ptm_u, &hos_frd_u, &hos_boss_u, &hos_fs_user, &hos_hid_user, &hos_gsp_gpu, &hos_dsp_dsp, &hos_ldr_ro, &hos_nwm_uds, &hos_cecd_u};

uint32_t hos_in(HosRequest* q, int k)
{
    return ld32(&q->thread->cpu, q->buffer + 4u * (uint32_t)k);
}

void hos_out(HosRequest* q, int k, uint32_t v)
{
    st32(&q->thread->cpu, q->buffer + 4u * (uint32_t)k, v);
}

void hos_reply(HosRequest* q, int normal, int translate)
{
    hos_out(q, 0, q->command << 16 | (uint32_t)normal << 6 | (uint32_t)translate);
}

int hos_out_handles(HosRequest* q, int k, int move, const uint32_t* handles, int n)
{
    hos_out(q, k, (uint32_t)(n - 1) << 26 | (move ? 0x10u : 0));
    for (int i = 0; i < n; i++)
        hos_out(q, k + 1 + i, handles[i]);
    return k + 1 + n;
}

int hos_out_static(HosRequest* q, int k, int id, const void* data, uint32_t size)
{
    Cpu* c = &q->thread->cpu;
    uint32_t target = ld32(c, q->buffer + 0x100u + 8u * (uint32_t)id + 4u);
    if (size)
        memcpy(hos_memory + target, data, size);
    hos_out(q, k, size << 14 | (uint32_t)id << 10 | 2u);
    hos_out(q, k + 1, target);
    return k + 2;
}

uint32_t hos_in_static(HosRequest* q, int k, uint32_t* size)
{
    *size = hos_in(q, k) >> 14;
    return hos_in(q, k + 1);
}

const HosService* hos_service_find(const char* name)
{
    for (size_t i = 0; i < sizeof services / sizeof services[0]; i++)
        if (!strcmp(services[i]->name, name))
            return services[i];
    return NULL;
}

uint32_t hos_session_open(const HosService* s)
{
    HosObject* o = hos_object_new(HOS_SESSION);
    o->service = s;
    uint32_t h = hos_handle_new(o);
    hos_object_release(o);
    return h;
}

HosResult hos_connect_port(const char* name, uint32_t* handle)
{
    if (!strcmp(name, "srv:")) {
        *handle = hos_session_open(&hos_srv);
        return HOS_OK;
    }
    fprintf(stderr, "hos: no port %s\n", name);
    return HOS_ERR_NOT_FOUND;
}

HosResult hos_ipc_request(HosThread* t, HosObject* session)
{
    HosRequest q = {t, session, t->cpu.tls + 0x80, 0};
    q.command = hos_in(&q, 0) >> 16;
    const HosService* s = session->service;
    static int trace = -1;
    if (trace < 0)
        trace = getenv("HOS_TRACE") != NULL;
    for (int i = 0; s->commands[i].run; i++)
        if (s->commands[i].id == q.command) {
            if (trace)
                fprintf(stderr, "trace thread %u ipc %s:%s %08X %08X %08X %08X %08X\n", t->id, s->name, s->commands[i].name,
                        hos_in(&q, 0), hos_in(&q, 1), hos_in(&q, 2), hos_in(&q, 3), hos_in(&q, 4));
            s->commands[i].run(&q);
            return HOS_OK;
        }
    fprintf(stderr, "hos: %s command %04X not done (header %08X, thread %u, lr %08X)\n", s->name, q.command,
            hos_in(&q, 0), t->id, t->cpu.r[14]);
    t->cpu.fault = HOS_FAULT_SERVICE;
    return HOS_OK;
}
