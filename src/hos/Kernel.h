/* The 3DS kernel as the translated game sees it (docs/RECOMPILATION_PLAN.md §3, step 3): its objects, the handles
 * naming them, the threads and their scheduling, the system calls (hos_svc). The rules are Azahar's
 * (azahar/src/core/hle/kernel/), which follow the real kernel's: where this differs, the comment says so.
 *
 * Threads: each game thread is a host thread running the translated code, but only one runs game code at a time
 * (the 3DS gives an application one core): the one holding the CPU (hos_current). A thread gives the CPU up only in a
 * system call that blocks it or wakes a thread of higher priority (a lower number, 0 highest), as on the 3DS, where
 * an application thread is only preempted by such a thread becoming ready. Waits with a timeout and timers are kept
 * by a clock thread (Thread.c), real time.
 */
#ifndef HOS_KERNEL_H
#define HOS_KERNEL_H

#include <pthread.h>
#include <stdint.h>

#include "Cpu.h"

typedef uint32_t HosResult;
enum {
    HOS_OK = 0,
    /* Azahar's kernel/errors.h */
    HOS_ERR_INVALID_HANDLE = 0xD8E007F7,
    HOS_ERR_TIMEOUT = 0x09401BFE,
    HOS_ERR_OUT_OF_MEMORY = 0xD86007F3,
    HOS_ERR_INVALID_ADDRESS = 0xE0E01BF5,
    HOS_ERR_NOT_FOUND = 0xD88007FA,
    HOS_ERR_INVALID_ENUM = 0xE0E01BEE,
    HOS_ERR_OUT_OF_RANGE = 0xE0E01BFD,
    HOS_ERR_NOT_IMPLEMENTED = 0xF8C007F4,
};

typedef enum { HOS_EVENT, HOS_MUTEX, HOS_SEMAPHORE, HOS_TIMER, HOS_THREAD, HOS_ARBITER, HOS_SESSION, HOS_PORT,
               HOS_MEMORY_BLOCK, HOS_PROCESS, HOS_RESOURCE_LIMIT } HosKind;

typedef enum { HOS_RESET_ONESHOT = 0, HOS_RESET_STICKY = 1, HOS_RESET_PULSE = 2 } HosReset;

struct HosThread;

/* a kernel object: what a handle names. Waitable ones (events, mutexes, semaphores, timers, threads) keep the threads
   waiting on them. */
typedef struct HosObject {
    HosKind kind;
    int refs;
    /* event, timer */
    int signaled, reset;
    int64_t timer_initial, timer_interval, timer_due; /* ns; due: host time of the next firing, 0 when off */
    /* mutex */
    struct HosThread* owner;
    int lock_count;
    /* semaphore */
    int32_t count, max_count;
    /* thread: the thread it is */
    struct HosThread* thread;
    /* session: the service (Ipc.c) */
    const struct HosService* service;
    void* service_data;
    /* memory block */
    uint32_t block_address, block_size;
    /* waiters (a fixed list: the 3DS game has tens of threads) */
    struct HosThread* waiters[64];
    int waiter_count;
    char name[12];
} HosObject;

typedef enum { HOS_READY, HOS_RUNNING, HOS_WAITING, HOS_DEAD } HosThreadState;

typedef struct HosThread {
    Cpu cpu;
    uint32_t id;
    int priority;
    HosThreadState state;
    pthread_t host;
    pthread_cond_t wake;
    uint32_t entry, arg, stack_top;
    HosObject* object; /* the thread's own object (what WaitSynchronization on its handle waits on) */
    /* what it waits on */
    HosObject* waits[64];
    int wait_count, wait_all;
    int woken_by; /* the index in waits of the object that woke it, -1 for a timeout */
    int64_t deadline; /* host time (ns) its wait times out, 0 for none */
    uint32_t arbiter_address; /* waiting on an address arbiter: the address */
    HosObject* arbiter;
    struct HosThread* next_ready;
} HosThread;

/* the CPU's holder and the kernel's lock (every kernel structure is touched under it) */
extern pthread_mutex_t hos_lock;
extern HosThread* hos_current;

/* handles */
uint32_t hos_handle_new(HosObject* o);
HosObject* hos_handle_get(uint32_t handle, HosThread* t);
HosResult hos_handle_close(uint32_t handle);
HosObject* hos_object_new(HosKind kind);
void hos_object_release(HosObject* o);
enum { HOS_HANDLE_CURRENT_THREAD = 0xFFFF8000u, HOS_HANDLE_CURRENT_PROCESS = 0xFFFF8001u };

/* time */
int64_t hos_now_ns(void);
uint64_t hos_ticks(void); /* the 3DS's system tick: 268,111,856 a second (Azahar's BASE_CLOCK_RATE_ARM11) */

/* threads (Thread.c): all under hos_lock */
HosThread* hos_thread_create(uint32_t entry, uint32_t arg, uint32_t stack_top, int priority, uint32_t tls);
void hos_thread_start_main(HosThread* t); /* the first thread: takes the CPU and runs, on the calling host thread */
void hos_make_ready(HosThread* t);        /* t becomes ready; the CPU goes to it if it outranks the current one */
void hos_block(HosThread* t);             /* the current thread gives the CPU up until made ready again */
void hos_yield(HosThread* t);             /* SleepThread(0): behind the ready threads of its priority */
void hos_thread_exit(HosThread* t);
void hos_clock_start(void);               /* the clock thread: timeouts and timers */
void hos_clock_kick(void);                /* a new deadline: the clock thread looks again */

/* waiting (Sync.c) */
HosResult hos_wait(HosThread* t, HosObject** objects, int count, int wait_all, int64_t timeout_ns, int32_t* index);
int hos_object_ready_for(HosObject* o, HosThread* t);
void hos_object_acquire(HosObject* o, HosThread* t);
void hos_object_wake(HosObject* o); /* o's state changed: wake the waiters it now satisfies */
void hos_timers_fire(int64_t now);
int64_t hos_next_deadline(void);
void hos_timeouts_fire(int64_t now);

/* the system calls (Kernel.c) */
void hos_svc(Cpu* c, uint32_t n);
HosThread* hos_thread_of(Cpu* c);

/* the translated game's lookup of a function by address (generated by tools/build_game.py) */
void call_indirect(Cpu* c, uint32_t t, uint32_t ret);

#endif
