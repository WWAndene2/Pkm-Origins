/* Waiting on kernel objects, and the objects' rules (Kernel.h), as Azahar's kernel has them (event.cpp, mutex.cpp,
 * semaphore.cpp, timer.cpp, address_arbiter.cpp, svc.cpp's WaitSynchronization1 / N):
 *   event, timer   ready while signaled; taking a one-shot one clears it; a pulse one is cleared once it has woken
 *                  its waiters
 *   mutex          ready while free or held by the waiter (it nests); released by its holder only
 *   semaphore      ready while its count is above 0; taking it decrements it
 *   thread         ready once it has ended
 * Not done (Azahar has it): a mutex raising its holder's priority to its waiters' (priority inheritance).
 * Everything here runs under hos_lock.
 */
#include "Kernel.h"

#include <stdlib.h>
#include <string.h>

/* every thread, for the timeouts (the game makes some hundred over a session, most of them short-lived) */
static HosThread* waiting_with_deadline[256];
static int deadlines;
static HosObject* timers[64];
static int timer_count;

int hos_object_ready_for(HosObject* o, HosThread* t)
{
    switch (o->kind) {
    case HOS_EVENT:
    case HOS_TIMER:
        return o->signaled;
    case HOS_MUTEX:
        return o->lock_count == 0 || o->owner == t;
    case HOS_SEMAPHORE:
        return o->count > 0;
    case HOS_THREAD:
        return o->thread->state == HOS_DEAD;
    default:
        return 0;
    }
}

void hos_object_acquire(HosObject* o, HosThread* t)
{
    switch (o->kind) {
    case HOS_EVENT:
    case HOS_TIMER:
        if (o->reset == HOS_RESET_ONESHOT)
            o->signaled = 0;
        break;
    case HOS_MUTEX:
        o->owner = t;
        o->lock_count++;
        break;
    case HOS_SEMAPHORE:
        if (o->count > 0)
            o->count--;
        break;
    default:
        break;
    }
}

static void add_waiter(HosObject* o, HosThread* t)
{
    if (o->waiter_count < 64)
        o->waiters[o->waiter_count++] = t;
}

static void remove_waiter(HosObject* o, HosThread* t)
{
    for (int i = 0; i < o->waiter_count; i++)
        if (o->waiters[i] == t) {
            memmove(&o->waiters[i], &o->waiters[i + 1], (size_t)(o->waiter_count - i - 1) * sizeof o->waiters[0]);
            o->waiter_count--;
            return;
        }
}

static void forget_deadline(HosThread* t)
{
    for (int i = 0; i < deadlines; i++)
        if (waiting_with_deadline[i] == t) {
            waiting_with_deadline[i] = waiting_with_deadline[--deadlines];
            break;
        }
    t->deadline = 0;
}

/* t stops waiting: off every object's list, its timeout forgotten, ready */
static void end_wait(HosThread* t, int woken_by)
{
    for (int i = 0; i < t->wait_count; i++)
        remove_waiter(t->waits[i], t);
    t->wait_count = 0;
    t->woken_by = woken_by;
    forget_deadline(t);
    hos_make_ready(t);
}

static int all_ready(HosThread* t)
{
    for (int i = 0; i < t->wait_count; i++)
        if (!hos_object_ready_for(t->waits[i], t))
            return 0;
    return 1;
}

void hos_object_wake(HosObject* o)
{
    /* (a copy of the list: waking a thread can hand the CPU over and change it) */
    HosThread* list[64];
    int n = o->waiter_count;
    memcpy(list, o->waiters, (size_t)n * sizeof list[0]);
    for (int k = 0; k < n; k++) {
        HosThread* t = list[k];
        if (t->state != HOS_WAITING || !hos_object_ready_for(o, t))
            continue;
        if (t->wait_all) {
            if (!all_ready(t))
                continue;
            for (int i = 0; i < t->wait_count; i++)
                hos_object_acquire(t->waits[i], t);
            end_wait(t, 0);
        } else {
            int index = 0;
            while (index < t->wait_count && t->waits[index] != o)
                index++;
            hos_object_acquire(o, t);
            end_wait(t, index);
        }
    }
    if (o->kind == HOS_EVENT && o->reset == HOS_RESET_PULSE)
        o->signaled = 0;
}

HosResult hos_wait(HosThread* t, HosObject** objects, int count, int wait_all, int64_t timeout_ns, int32_t* index)
{
    if (wait_all) {
        int ready = 1;
        for (int i = 0; i < count; i++)
            ready = ready && hos_object_ready_for(objects[i], t);
        if (ready) {
            for (int i = 0; i < count; i++)
                hos_object_acquire(objects[i], t);
            if (index)
                *index = -1;
            return HOS_OK;
        }
    } else {
        for (int i = 0; i < count; i++)
            if (hos_object_ready_for(objects[i], t)) {
                hos_object_acquire(objects[i], t);
                if (index)
                    *index = i;
                return HOS_OK;
            }
    }
    if (timeout_ns == 0)
        return HOS_ERR_TIMEOUT;
    t->wait_count = count;
    t->wait_all = wait_all;
    for (int i = 0; i < count; i++) {
        t->waits[i] = objects[i];
        add_waiter(objects[i], t);
    }
    if (timeout_ns > 0) {
        t->deadline = hos_now_ns() + timeout_ns;
        if (deadlines < 256)
            waiting_with_deadline[deadlines++] = t;
        hos_clock_kick();
    }
    hos_block(t);
    if (t->woken_by < 0)
        return HOS_ERR_TIMEOUT;
    if (index)
        *index = wait_all ? -1 : t->woken_by;
    return HOS_OK;
}

void hos_timeouts_fire(int64_t now)
{
    for (int i = 0; i < deadlines;) {
        HosThread* t = waiting_with_deadline[i];
        if (t->deadline && t->deadline <= now) {
            if (t->arbiter) { /* (an arbiter wait: off the arbiter's list) */
                remove_waiter(t->arbiter, t);
                t->arbiter = NULL;
            }
            end_wait(t, -1); /* (removes it from this list) */
        } else {
            i++;
        }
    }
}

/* timers: a set timer fires at its due time, signaling (and waking) as an event does, then again every interval */
void hos_timer_set(HosObject* o, int64_t initial, int64_t interval)
{
    o->timer_initial = initial;
    o->timer_interval = interval;
    o->timer_due = hos_now_ns() + initial;
    int known = 0;
    for (int i = 0; i < timer_count; i++)
        known = known || timers[i] == o;
    if (!known && timer_count < 64)
        timers[timer_count++] = o;
    hos_clock_kick();
}

void hos_timers_fire(int64_t now)
{
    for (int i = 0; i < timer_count; i++) {
        HosObject* o = timers[i];
        if (o->timer_due && o->timer_due <= now) {
            o->signaled = 1;
            hos_object_wake(o);
            if (o->reset == HOS_RESET_PULSE)
                o->signaled = 0;
            o->timer_due = o->timer_interval > 0 ? o->timer_due + o->timer_interval : 0;
        }
    }
}

int64_t hos_next_deadline(void)
{
    int64_t next = 0;
    for (int i = 0; i < deadlines; i++)
        if (!next || waiting_with_deadline[i]->deadline < next)
            next = waiting_with_deadline[i]->deadline;
    for (int i = 0; i < timer_count; i++)
        if (timers[i]->timer_due && (!next || timers[i]->timer_due < next))
            next = timers[i]->timer_due;
    return next;
}

/* the address arbiter (address_arbiter.cpp): waits on a word of memory, woken by a signal on that address */
HosResult hos_arbitrate(HosThread* t, HosObject* arbiter, uint32_t address, uint32_t type, int32_t value,
                        int64_t timeout_ns)
{
    switch (type) {
    case 0: { /* signal: the first value waiters by priority (all when value < 0) */
        for (int woken = 0; value < 0 || woken < value; woken++) {
            HosThread* best = NULL;
            for (int i = 0; i < arbiter->waiter_count; i++) {
                HosThread* w = arbiter->waiters[i];
                if (w->arbiter_address == address && (!best || w->priority < best->priority))
                    best = w;
            }
            if (!best)
                break;
            remove_waiter(arbiter, best);
            best->arbiter = NULL;
            end_wait(best, 0);
        }
        return HOS_OK;
    }
    case 1:   /* wait if less than */
    case 2:   /* decrement and wait if less than */
    case 3:   /* wait if less than, with a timeout */
    case 4: { /* decrement and wait if less than, with a timeout */
        Cpu* c = &t->cpu;
        int32_t v = (int32_t)ld32(c, address);
        if (v < value) {
            if (type == 2 || type == 4) /* (only when it waits) */
                st32(c, address, (uint32_t)(v - 1));
            t->arbiter_address = address;
            t->arbiter = arbiter;
            t->wait_count = 0;
            add_waiter(arbiter, t);
            if (type >= 3) {
                t->deadline = hos_now_ns() + timeout_ns;
                if (deadlines < 256)
                    waiting_with_deadline[deadlines++] = t;
                hos_clock_kick();
            }
            hos_block(t);
        }
        /* the calls with a timeout return Timeout even when they did not wait (Azahar's note) */
        return type >= 3 ? HOS_ERR_TIMEOUT : HOS_OK;
    }
    default:
        return HOS_ERR_INVALID_ENUM;
    }
}
