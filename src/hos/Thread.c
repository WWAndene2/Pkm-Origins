/* Threads and their scheduling (Kernel.h): one CPU, held by hos_current; ready threads in a queue per priority
 * (0-63, FIFO in each); a clock thread for timeouts and timers.
 *
 * As on the 3DS, a thread runs until it blocks, yields, or wakes a thread of higher priority in a system call (it
 * then gives the CPU to that thread at once, and goes back to the front of its own priority's queue). One difference:
 * a thread woken by the clock (a timeout, a timer) while another runs takes the CPU at that one's next system call,
 * not at once (the clock thread cannot stop translated code mid-way); the game makes system calls thousands of times
 * a second, and the 3DS's own timer interrupts are not instant either. Time is real (the host's monotonic clock).
 */
#include "Kernel.h"
#include "Cro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include <time.h>
#include <signal.h>

volatile int hos_preempt_pending; /* a thread outranking the running one made ready from outside its system calls */

pthread_mutex_t hos_lock = PTHREAD_MUTEX_INITIALIZER;
HosThread* hos_current;

static HosThread* all[512]; /* every thread made (for the state dump) */
static int all_count;
static HosThread* head[64];
static HosThread* tail[64];
static pthread_cond_t clock_cond = PTHREAD_COND_INITIALIZER;
static uint32_t next_id = 1;

/* the address a thread's entry function returns to: no code is there; reaching it ends the thread */
enum { THREAD_RETURN = 0xFFFFFFF0u };

/* the game's time: the host's monotonic clock, or with $HOS_TIME_SCALE=<factor> that clock slowed (0.1: ten times
   slower), so that a slow host (rendering on a software GPU) never makes the game drop frames: its frame n is then the
   frame n of a full-speed run, which the comparisons with Azahar need (tools/frame_compare.py) */
static double time_scale = -1;
static int64_t time_origin;

int64_t hos_now_ns(void)
{
#define scale time_scale
#define origin time_origin
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t real = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    if (scale < 0) {
        const char* f = getenv("HOS_TIME_SCALE");
        scale = f ? atof(f) : 1.0;
        if (scale <= 0)
            scale = 1.0;
        origin = real;
    }
    return scale == 1.0 ? real : origin + (int64_t)((double)(real - origin) * scale);
#undef scale
#undef origin
}

/* a game time as the host's monotonic time (for the clock thread's sleep) */
static int64_t host_ns(int64_t game)
{
    return time_scale <= 0 || time_scale == 1.0 ? game : time_origin + (int64_t)((double)(game - time_origin) / time_scale);
}

uint64_t hos_ticks(void)
{
    static int64_t start;
    if (!start)
        start = hos_now_ns();
    __int128 ns = hos_now_ns() - start;
    return (uint64_t)(ns * 268111856 / 1000000000);
}

static void enqueue(HosThread* t, int front)
{
    int p = t->priority;
    t->next_ready = NULL;
    if (!head[p]) {
        head[p] = tail[p] = t;
    } else if (front) {
        t->next_ready = head[p];
        head[p] = t;
    } else {
        tail[p]->next_ready = t;
        tail[p] = t;
    }
}

static void unqueue(HosThread* t)
{
    int p = t->priority;
    HosThread** at = &head[p];
    HosThread* prev = NULL;
    while (*at && *at != t) {
        prev = *at;
        at = &(*at)->next_ready;
    }
    if (*at) {
        *at = t->next_ready;
        if (tail[p] == t)
            tail[p] = prev;
    }
}

static HosThread* best_ready(void)
{
    for (int p = 0; p < 64; p++)
        if (head[p])
            return head[p];
    return NULL;
}

/* the CPU to the best ready thread, or to none */
static void dispatch(void)
{
    HosThread* n = best_ready();
    if (n) {
        unqueue(n);
        n->state = HOS_RUNNING;
    }
    hos_current = n;
    if (n)
        pthread_cond_signal(&n->wake);
}

static void wait_cpu(HosThread* t)
{
    int waited = 0;
    while (hos_current != t) {
        pthread_cond_wait(&t->wake, &hos_lock);
        waited = 1;
    }
    if (waited)
        t->cpu.mon = 0; /* the kernel's clrex on a thread switch: an ldrex from before the switch no longer lets the
                           strex after it succeed (another thread may have taken the lock in between) */
}

void hos_block(HosThread* t)
{
    t->state = HOS_WAITING;
    dispatch();
    wait_cpu(t);
}

void hos_yield(HosThread* t)
{
    HosThread* n = best_ready();
    if (!n || n->priority > t->priority)
        return; /* nothing of its priority or higher waits: it keeps the CPU */
    t->state = HOS_READY;
    enqueue(t, 0);
    dispatch();
    wait_cpu(t);
}

void hos_make_ready(HosThread* t)
{
    if (t->state != HOS_WAITING)
        return;
    t->state = HOS_READY;
    HosThread* cur = hos_current;
    if (!cur) {
        enqueue(t, 0);
        dispatch();
        return;
    }
    if (t->priority < cur->priority && pthread_equal(pthread_self(), cur->host)) {
        /* woken by the running thread in a system call, and outranking it: the CPU goes to it now */
        cur->state = HOS_READY;
        enqueue(cur, 1);
        t->state = HOS_RUNNING;
        hos_current = t;
        pthread_cond_signal(&t->wake);
        wait_cpu(cur);
        return;
    }
    enqueue(t, 0);
    if (t->priority < cur->priority)
        hos_preempt_pending = 1; /* (the running thread hands over the CPU at its next loop head or system call) */
}


/* a loop head of the translated code, the flag set: the CPU to the thread that outranks this one, if one is ready */
void hos_preempt_point(Cpu* c)
{
    pthread_mutex_lock(&hos_lock);
    hos_preempt_pending = 0;
    HosThread* t = hos_thread_of(c);
    if (t && hos_current == t)
        hos_preempt_check(t);
    pthread_mutex_unlock(&hos_lock);
}

/* called at the end of each system call: a thread the clock woke that outranks the caller takes the CPU */
void hos_preempt_check(HosThread* t)
{
    HosThread* n = best_ready();
    if (n && n->priority < t->priority) {
        t->state = HOS_READY;
        enqueue(t, 1);
        dispatch();
        wait_cpu(t);
    }
}

static void run(HosThread* t)
{
    Cpu* c = &t->cpu;
    fesetround(FE_TOWARDZERO); /* the host's rounding as the thread's FPSCR (0x03C00010: toward zero) */
    c->r[14] = THREAD_RETURN;
    call_indirect(c, t->entry, THREAD_RETURN);
    /* a return to an address no frame above was called from (fault 3, c->unwind): the C frames are all gone, and
       the CPU just goes on there; so does the thread, at that address's code when there is some (ORAS's thread
       exit: its last function run on a stack of its own returns to 00131FD8, the svc ExitThread of the start
       routine). Nothing there: the fault stays, reported below. */
    while (c->fault == 3 && c->unwind) {
        const uint32_t to = c->unwind;
        const int keep_fault = c->fault;
        const uint32_t keep_pc = c->r[15];
        c->fault = 0;
        c->unwind = 0;
        call_indirect(c, to, THREAD_RETURN);
        if (c->fault == 4) { /* (no code translated there: the original fault) */
            c->fault = keep_fault;
            c->unwind = to;
            c->r[15] = keep_pc;
            break;
        }
    }
    if (c->fault) {
        char at[96] = "", from[96] = "";
        hos_ldr_place(c->r[15], at, sizeof at);
        hos_ldr_place(c->r[14], from, sizeof from);
        fprintf(stderr, "hos: thread %u stopped: fault %d at %08X%s%s%s (lr %08X%s%s%s, sp %08X)\n", t->id, c->fault,
                c->r[15], at[0] ? " (" : "", at, at[0] ? ")" : "", c->r[14], from[0] ? " " : "", from, "",
                c->r[13]);
        if (c->fault == 3) /* (the address it returned to, that no frame above was called from) */
            fprintf(stderr, "hos: thread %u returned to %08X, which no caller above called from\n", t->id, c->unwind);
        if (c->fault != 100) /* (100: ExitThread, ExitProcess: a normal end) */
            fflush(stderr);
    }
}

static void* host_main(void* p)
{
    HosThread* t = p;
    pthread_mutex_lock(&hos_lock);
    wait_cpu(t);
    pthread_mutex_unlock(&hos_lock);
    run(t);
    pthread_mutex_lock(&hos_lock);
    hos_thread_exit(t);
    pthread_mutex_unlock(&hos_lock);
    return NULL;
}

HosThread* hos_thread_create(uint32_t entry, uint32_t arg, uint32_t stack_top, int priority, uint32_t tls)
{
    HosThread* t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->id = next_id++;
    t->priority = priority;
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    t->cpu.r[0] = arg;
    t->cpu.r[13] = stack_top;
    t->cpu.fpscr = 0x03C00010u; /* as the kernel sets every thread's (Azahar's kernel/thread.cpp) */
    t->cpu.tls = tls;
    t->woken_by = -1;
    pthread_cond_init(&t->wake, NULL);
    t->object = hos_object_new(HOS_THREAD);
    t->object->thread = t;
    t->state = HOS_WAITING;
    if (all_count < 512)
        all[all_count++] = t;
    return t;
}

int hos_thread_launch(HosThread* t)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20); /* the translated code's C frames: deep call chains */
    int r = pthread_create(&t->host, &attr, host_main, t);
    pthread_attr_destroy(&attr);
    if (r)
        return -1;
    hos_make_ready(t);
    return 0;
}

void hos_thread_start_main(HosThread* t)
{
    pthread_mutex_lock(&hos_lock);
    t->host = pthread_self();
    t->state = HOS_RUNNING;
    hos_current = t;
    pthread_mutex_unlock(&hos_lock);
    run(t);
    pthread_mutex_lock(&hos_lock);
    hos_thread_exit(t);
    pthread_mutex_unlock(&hos_lock);
}

void hos_thread_exit(HosThread* t)
{
    t->state = HOS_DEAD;
    hos_release_thread_mutexes(t);
    t->object->signaled = 1; /* a thread's object is signaled once it has ended */
    hos_object_wake(t->object);
    if (hos_current == t)
        dispatch();
}

/* periodic work of the system (the GPU's VBlank: Gsp.c), run by the clock thread under hos_lock */
static struct { int64_t period, next; void (*run)(void); } periodic[8];
static int periodic_count;

void hos_clock_every(int64_t period_ns, void (*run)(void))
{
    if (periodic_count < 8) {
        periodic[periodic_count].period = period_ns;
        periodic[periodic_count].next = hos_now_ns() + period_ns;
        periodic[periodic_count++].run = run;
        hos_clock_kick();
    }
}

/* every live thread's state and what it waits on, to stderr ($HOS_DUMP_AFTER seconds after the start: a run that
   stops making progress) */
static void dump_threads(void)
{
    static const char* states[4] = {"ready", "running", "waiting", "dead"};
    static const char* kinds[11] = {"event", "mutex", "semaphore", "timer", "thread", "arbiter", "session", "port",
                                    "memory block", "process", "resource limit"};
    fprintf(stderr, "hos: threads (%d made):\n", all_count);
    for (int i = 0; i < all_count; i++) {
        HosThread* t = all[i];
        if (t->state == HOS_DEAD)
            continue;
        fprintf(stderr, "  thread %u priority %d %s pc %08X lr %08X", t->id, t->priority, states[t->state],
                t->cpu.r[15], t->cpu.r[14]);
        for (int k = 0; k < t->wait_count; k++)
            fprintf(stderr, " | waits on %s %s", kinds[t->waits[k]->kind], t->waits[k]->name);
        if (t->arbiter)
            fprintf(stderr, " | arbiter address %08X", t->arbiter_address);
        if (t->deadline)
            fprintf(stderr, " | with a timeout");
        fprintf(stderr, "\n");
    }
}

/* SIGUSR1: the threads dumped at the clock's next tick (within 0.1 s; tools/hang_watch.sh sends it on a hang) */
static volatile sig_atomic_t dump_requested;
static void on_dump_signal(int s) { (void)s; dump_requested = 1; }

/* the clock thread: wakes the waits whose timeout passed and fires the timers due, then sleeps to the next one */
static void* clock_main(void* p)
{
    (void)p;
    pthread_mutex_lock(&hos_lock);
    signal(SIGUSR1, on_dump_signal);
    const char* d = getenv("HOS_DUMP_AFTER");
    int64_t dump_at = d ? hos_now_ns() + (int64_t)(atof(d) * 1e9) : 0;
    for (;;) {
        int64_t now = hos_now_ns();
        if ((dump_at && now >= dump_at) || dump_requested) {
            dump_threads();
            dump_at = 0;
            dump_requested = 0;
        }
        hos_timeouts_fire(now);
        hos_timers_fire(now);
        for (int i = 0; i < periodic_count; i++)
            while (periodic[i].next <= now) {
                periodic[i].run();
                periodic[i].next += periodic[i].period;
            }
        int64_t next = hos_next_deadline();
        for (int i = 0; i < periodic_count; i++)
            if (!next || periodic[i].next < next)
                next = periodic[i].next;
        if (!next || next - now > 100000000)
            next = now + 100000000; /* (at least every 0.1 s) */
        const int64_t wake = host_ns(next);
        struct timespec ts = {(time_t)(wake / 1000000000), (long)(wake % 1000000000)};
        pthread_cond_timedwait(&clock_cond, &hos_lock, &ts);
    }
    return NULL;
}

void hos_clock_start(void)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_destroy(&clock_cond);
    pthread_cond_init(&clock_cond, &a);
    pthread_t th;
    pthread_create(&th, NULL, clock_main, NULL);
}

void hos_clock_kick(void)
{
    pthread_cond_signal(&clock_cond);
}
