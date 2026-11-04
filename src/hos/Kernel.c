/* The system calls (svc) and the handle table (Kernel.h). Each call's registers are Azahar's ABI as tools/svc_abi.py
 * computes it from azahar/src/core/hle/kernel/svc.cpp (inputs, outputs: r0 the result, then each output word); the
 * comment on each case repeats them. The calls the two games make (tools/svc_inventory.py, tools/hos_calls.py) are
 * here; any other stops the thread (fault 11, the call's number in r0) rather than answer something made up.
 */
#include "Kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Memory.h"

void hos_preempt_check(HosThread* t);
int hos_thread_launch(HosThread* t);
HosResult hos_arbitrate(HosThread* t, HosObject* arbiter, uint32_t address, uint32_t type, int32_t value,
                        int64_t timeout_ns);
void hos_timer_set(HosObject* o, int64_t initial, int64_t interval);
HosResult hos_ipc_request(HosThread* t, HosObject* session);
HosResult hos_connect_port(const char* name, uint32_t* handle);
HosResult hos_control_memory(uint32_t* out, uint32_t op, uint32_t addr0, uint32_t addr1, uint32_t size, uint32_t perm);

enum { FAULT_UNKNOWN_SVC = 11, FAULT_EXIT = 100 };

/* handles: an index into the table (from 1), as Azahar's handle_table.cpp numbers them (index | generation << 15) */
static HosObject* table[4096];
/* how many objects of each resource kind exist (resource_limit.h's numbering), for GetResourceLimitCurrentValues */
int64_t hos_object_count[16];
static int resource_kind(HosKind k)
{
    switch (k) {
    case HOS_THREAD: return 2;
    case HOS_EVENT: return 3;
    case HOS_MUTEX: return 4;
    case HOS_SEMAPHORE: return 5;
    case HOS_TIMER: return 6;
    case HOS_MEMORY_BLOCK: return 7;
    case HOS_ARBITER: return 8;
    default: return 0;
    }
}
static uint16_t generation[4096];

HosObject* hos_object_new(HosKind kind)
{
    HosObject* o = calloc(1, sizeof *o);
    o->kind = kind;
    o->refs = 1;
    hos_object_count[resource_kind(kind)]++;
    return o;
}

void hos_object_release(HosObject* o)
{
    if (o && --o->refs == 0 && o->kind != HOS_THREAD) { /* (a thread's object lives as long as its thread) */
        hos_object_count[resource_kind(o->kind)]--;
        free(o);
    }
}

uint32_t hos_handle_new(HosObject* o)
{
    for (uint32_t i = 1; i < 4096; i++)
        if (!table[i]) {
            table[i] = o;
            generation[i] = (uint16_t)((generation[i] + 1) & 0x7FFF);
            if (!generation[i])
                generation[i] = 1;
            o->refs++;
            return i | (uint32_t)generation[i] << 15;
        }
    return 0;
}

HosObject* hos_handle_get(uint32_t handle, HosThread* t)
{
    if (handle == HOS_HANDLE_CURRENT_THREAD)
        return t->object;
    uint32_t i = handle & 0x7FFF;
    if (i == 0 || i >= 4096 || !table[i] || generation[i] != (handle >> 15))
        return NULL;
    return table[i];
}

HosResult hos_handle_close(uint32_t handle)
{
    uint32_t i = handle & 0x7FFF;
    if (i == 0 || i >= 4096 || !table[i] || generation[i] != (handle >> 15))
        return HOS_ERR_INVALID_HANDLE;
    hos_object_release(table[i]);
    table[i] = NULL;
    return HOS_OK;
}

/* the HosThread a Cpu belongs to (the Cpu is its first member) */
HosThread* hos_thread_of(Cpu* c)
{
    return (HosThread*)c;
}

static uint32_t next_tls = HOS_TLS;

static int64_t s64_of(uint32_t lo, uint32_t hi)
{
    return (int64_t)((uint64_t)hi << 32 | lo);
}

void hos_svc(Cpu* c, uint32_t n)
{
    HosThread* t = hos_thread_of(c);
    uint32_t* r = c->r;
    HosObject* o;
    pthread_mutex_lock(&hos_lock);
    switch (n) {
    case 0x01: /* ControlMemory: in r0=operation, r1=addr0, r2=addr1, r3=size, r4=permissions; out r0, r1=out_addr */
        r[0] = hos_control_memory(&r[1], r[0], r[1], r[2], r[3], r[4]);
        break;
    case 0x03: /* ExitProcess */
        fprintf(stderr, "hos: the game ended (ExitProcess)\n");
        exit(0);
    case 0x08: { /* CreateThread: in r0=priority, r1=entry_point, r2=arg, r3=stack_top, r4=processor_id; out r0, r1=handle */
        if (r[0] > 63) {
            r[0] = HOS_ERR_OUT_OF_RANGE;
            break;
        }
        next_tls += HOS_TLS_SIZE;
        HosThread* nt = hos_thread_create(r[1], r[2], r[3], (int)r[0], next_tls);
        r[1] = hos_handle_new(nt->object);
        r[0] = hos_thread_launch(nt) ? HOS_ERR_OUT_OF_MEMORY : HOS_OK;
        break;
    }
    case 0x09: /* ExitThread */
        c->fault = FAULT_EXIT; /* (the translated code returns all the way; Thread.c ends the thread) */
        break;
    case 0x0A: { /* SleepThread: in r0=ns.lo, r1=ns.hi; out - */
        int64_t ns = s64_of(r[0], r[1]);
        if (ns <= 0) {
            hos_yield(t);
        } else {
            static HosObject* never; /* (a wait on nothing that only the timeout ends) */
            if (!never)
                never = hos_object_new(HOS_EVENT);
            hos_wait(t, &never, 1, 0, ns, NULL);
        }
        break;
    }
    case 0x0B: /* GetThreadPriority: in r1=handle; out r0, r1=priority */
        o = hos_handle_get(r[1], t);
        if (!o || o->kind != HOS_THREAD) {
            r[0] = HOS_ERR_INVALID_HANDLE;
            break;
        }
        r[1] = (uint32_t)o->thread->priority;
        r[0] = HOS_OK;
        break;
    case 0x0C: /* SetThreadPriority: in r0=handle, r1=priority; out r0 */
        o = hos_handle_get(r[0], t);
        if (!o || o->kind != HOS_THREAD) {
            r[0] = HOS_ERR_INVALID_HANDLE;
            break;
        }
        if (r[1] > 63) {
            r[0] = HOS_ERR_OUT_OF_RANGE;
            break;
        }
        o->thread->priority = (int)r[1]; /* (a ready thread keeps its queue place until it next runs) */
        r[0] = HOS_OK;
        break;
    case 0x13: /* CreateMutex: in r1=initial_locked; out r0, r1=handle */
        o = hos_object_new(HOS_MUTEX);
        if (r[1]) {
            o->owner = t;
            o->lock_count = 1;
        }
        r[1] = hos_handle_new(o);
        hos_object_release(o);
        r[0] = HOS_OK;
        break;
    case 0x14: /* ReleaseMutex: in r0=handle; out r0 */
        o = hos_handle_get(r[0], t);
        if (!o || o->kind != HOS_MUTEX) {
            r[0] = HOS_ERR_INVALID_HANDLE;
            break;
        }
        if (o->owner != t || o->lock_count == 0) {
            r[0] = 0xD8E0041F; /* (Azahar: ERR_NOT_AUTHORIZED, a thread releasing a mutex it does not hold) */
            break;
        }
        if (--o->lock_count == 0) {
            o->owner = NULL;
            hos_object_wake(o);
        }
        r[0] = HOS_OK;
        break;
    case 0x17: /* CreateEvent: in r1=reset_type; out r0, r1=handle */
    case 0x1A: /* CreateTimer: in r1=reset_type; out r0, r1=handle */
        o = hos_object_new(n == 0x17 ? HOS_EVENT : HOS_TIMER);
        o->reset = (int)r[1];
        r[1] = hos_handle_new(o);
        hos_object_release(o);
        r[0] = HOS_OK;
        break;
    case 0x18: /* SignalEvent: in r0=handle; out r0 */
        o = hos_handle_get(r[0], t);
        if (!o || o->kind != HOS_EVENT) {
            r[0] = HOS_ERR_INVALID_HANDLE;
            break;
        }
        r[0] = HOS_OK;
        o->signaled = 1;
        hos_object_wake(o);
        break;
    case 0x19: /* ClearEvent: in r0=handle; out r0 */
        o = hos_handle_get(r[0], t);
        if (!o || o->kind != HOS_EVENT) {
            r[0] = HOS_ERR_INVALID_HANDLE;
            break;
        }
        o->signaled = 0;
        r[0] = HOS_OK;
        break;
    case 0x1B: /* SetTimer: in r0=handle, r1=interval.lo, r2=initial.lo, r3=initial.hi, r4=interval.hi; out r0 */
        o = hos_handle_get(r[0], t);
        if (!o || o->kind != HOS_TIMER) {
            r[0] = HOS_ERR_INVALID_HANDLE;
            break;
        }
        hos_timer_set(o, s64_of(r[2], r[3]), s64_of(r[1], r[4]));
        r[0] = HOS_OK;
        break;
    case 0x1C: /* CancelTimer: in r0=handle; out r0 */
        o = hos_handle_get(r[0], t);
        if (!o || o->kind != HOS_TIMER) {
            r[0] = HOS_ERR_INVALID_HANDLE;
            break;
        }
        o->timer_due = 0;
        r[0] = HOS_OK;
        break;
    case 0x21: /* CreateAddressArbiter: out r0, r1=handle */
        o = hos_object_new(HOS_ARBITER);
        r[1] = hos_handle_new(o);
        hos_object_release(o);
        r[0] = HOS_OK;
        break;
    case 0x22: /* ArbitrateAddress: in r0=handle, r1=address, r2=type, r3=value, r4=ns.lo, r5=ns.hi; out r0 */
        o = hos_handle_get(r[0], t);
        if (!o || o->kind != HOS_ARBITER) {
            r[0] = HOS_ERR_INVALID_HANDLE;
            break;
        }
        r[0] = hos_arbitrate(t, o, r[1], r[2], (int32_t)r[3], s64_of(r[4], r[5]));
        break;
    case 0x23: /* CloseHandle: in r0=handle; out r0 */
        r[0] = hos_handle_close(r[0]);
        break;
    case 0x24: { /* WaitSynchronization1: in r0=handle, r2=ns.lo, r3=ns.hi; out r0 */
        o = hos_handle_get(r[0], t);
        if (!o) {
            r[0] = HOS_ERR_INVALID_HANDLE;
            break;
        }
        r[0] = hos_wait(t, &o, 1, 0, s64_of(r[2], r[3]), NULL);
        break;
    }
    case 0x25: { /* WaitSynchronizationN: in r0=ns.lo, r1=handles, r2=count, r3=wait_all, r4=ns.hi; out r0, r1=index */
        HosObject* objs[64];
        int count = (int)r[2];
        if (count < 0 || count > 64) {
            r[0] = HOS_ERR_OUT_OF_RANGE;
            break;
        }
        int bad = 0;
        for (int i = 0; i < count; i++) {
            objs[i] = hos_handle_get(ld32(c, r[1] + 4u * (uint32_t)i), t);
            bad = bad || !objs[i];
        }
        if (bad) {
            r[0] = HOS_ERR_INVALID_HANDLE;
            break;
        }
        int32_t index = -1;
        r[0] = hos_wait(t, objs, count, r[3] != 0, s64_of(r[0], r[4]), &index);
        r[1] = (uint32_t)index;
        break;
    }
    case 0x27: /* DuplicateHandle: in r1=handle; out r0, r1=handle */
        o = hos_handle_get(r[1], t);
        if (!o) {
            r[0] = HOS_ERR_INVALID_HANDLE;
            break;
        }
        r[1] = hos_handle_new(o);
        r[0] = HOS_OK;
        break;
    case 0x28: { /* GetSystemTick: out r0, r1 = the tick (64 bits) */
        uint64_t k = hos_ticks();
        r[0] = (uint32_t)k;
        r[1] = (uint32_t)(k >> 32);
        break;
    }
    case 0x2D: { /* ConnectToPort: in r1=name address; out r0, r1=handle */
        char name[12] = {0};
        for (int i = 0; i < 11; i++)
            name[i] = (char)ld8(c, r[1] + (uint32_t)i);
        r[0] = hos_connect_port(name, &r[1]);
        break;
    }
    case 0x32: /* SendSyncRequest: in r0=handle; out r0 */
        o = hos_handle_get(r[0], t);
        if (!o || o->kind != HOS_SESSION) {
            r[0] = HOS_ERR_INVALID_HANDLE;
            break;
        }
        r[0] = hos_ipc_request(t, o);
        break;
    case 0x35: /* GetProcessId: in r1=process handle; out r0, r1=id */
        r[1] = 0x28; /* (Azahar's first application process id) */
        r[0] = HOS_OK;
        break;
    case 0x37: /* GetThreadId: in r1=handle; out r0, r1=id */
        o = hos_handle_get(r[1], t);
        if (!o || o->kind != HOS_THREAD) {
            r[0] = HOS_ERR_INVALID_HANDLE;
            break;
        }
        r[1] = o->thread->id;
        r[0] = HOS_OK;
        break;
    case 0x38: /* GetResourceLimit: in r1=process handle; out r0, r1=handle */
        o = hos_object_new(HOS_RESOURCE_LIMIT);
        r[1] = hos_handle_new(o);
        hos_object_release(o);
        r[0] = HOS_OK;
        break;
    case 0x3A: { /* GetResourceLimitCurrentValues: in r0=values, r1=handle, r2=names, r3=count; out r0 */
        /* each asked value, 64 bits: the current use of the resource (resource_limit.h's kinds: 1 the memory
           committed, 2 threads, 3 events, 4 mutexes, 5 semaphores, 6 timers, 7 shared memory, 8 arbiters) */
        for (uint32_t i = 0; i < r[3]; i++) {
            uint32_t kind = ld32(c, r[2] + 4 * i);
            int64_t v = kind == 1 ? (int64_t)hos_memory_committed() : kind < 16 ? hos_object_count[kind] : 0;
            st32(c, r[0] + 8 * i, (uint32_t)v);
            st32(c, r[0] + 8 * i + 4, (uint32_t)((uint64_t)v >> 32));
        }
        r[0] = HOS_OK;
        break;
    }
    case 0x3C: /* Break: in r0=reason */
        fprintf(stderr, "hos: the game called Break (%u) at %08X\n", r[0], r[14]);
        c->fault = FAULT_UNKNOWN_SVC;
        break;
    case 0x3D: { /* OutputDebugString: in r0=address, r1=length */
        fprintf(stderr, "hos: the game says: ");
        for (uint32_t i = 0; i < r[1] && i < 256; i++)
            fputc(ld8(c, r[0] + i), stderr);
        fputc('\n', stderr);
        break;
    }
    default:
        fprintf(stderr, "hos: system call %02X not done (thread %u, lr %08X)\n", n, t->id, r[14]);
        r[0] = n;
        c->fault = FAULT_UNKNOWN_SVC;
        break;
    }
    if (!c->fault)
        hos_preempt_check(t);
    pthread_mutex_unlock(&hos_lock);
}
