/* Counts every system call and service request a game makes in a headless Azahar run (Pomegrade's retro_host and
   core), for tools/hos_calls.py: loaded first (LD_PRELOAD), it waits for the core to be loaded, turns on Pomegrade's
   code trace (azahar/src/core/pomegrade_code_trace.h), whose hooks are at every svc and service request, and redirects
   those hooks to counters here: the trace's own record keeps only the last 131,072 calls (X's boot makes more before
   its first write), this keeps the count of all. The trace's thread snapshots (4 a second, every thread's stack read)
   are turned off. Nothing in Pomegrade is changed; the redirection is an absolute jump written over the first bytes of
   the core's functions in memory (x86-64 only).
   Written to $PKM_TRACE_DIR/counts.txt every second and when the run ends (or is stopped by SIGTERM / SIGINT):
     svc <id> <name> <count>        and        ipc <service>:<request> <header> <count>
   and to modules.txt each CRO module as it is loaded: <name> <address> <end> <calls made before it>

     cc -shared -fPIC -O1 -o azahar_trace_preload.so tools/azahar_trace_preload.c -ldl -lpthread
     PKM_TRACE_DIR=<folder> LD_PRELOAD=<that .so> <Pomegrade's run_local.sh ...>    (tools/azahar_trace_run.sh)
*/
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* libstdc++'s std::string of the C++11 ABI, as the Linux build of the core has it: pointer, size, then a 16-byte local
   buffer or the capacity (Configure takes one, ServiceRequest gets the service's name as one) */
struct cxx_string {
    char* p;
    size_t n;
    union {
        char local[16];
        size_t capacity;
    };
};

#define KEYS 4096
static struct { char key[128]; unsigned long long n; } table[KEYS];
static int used;
static unsigned long long total;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char out_path[4096];
static time_t last_write;

static void write_counts(void)
{
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
    FILE* f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "# calls %llu\n", total);
    for (int i = 0; i < used; i++)
        fprintf(f, "%s %llu\n", table[i].key, table[i].n);
    fclose(f);
    rename(tmp, out_path);
}

static void count(const char* key)
{
    pthread_mutex_lock(&lock);
    total++;
    int i;
    for (i = 0; i < used && strcmp(table[i].key, key); i++)
        ;
    if (i == used && used < KEYS) {
        snprintf(table[i].key, sizeof table[i].key, "%s", key);
        used++;
    }
    if (i < used)
        table[i].n++;
    time_t now = time(NULL);
    if (now != last_write) {
        last_write = now;
        write_counts();
    }
    pthread_mutex_unlock(&lock);
}

/* the core's KernelCall(Core::System&, u32 id, const char* name) and ServiceRequest(Core::System&, const std::string&
   service, const char* request, const u32* command, std::size_t words) */
static void kernel_call(void* system, uint32_t id, const char* name)
{
    (void)system;
    char key[128];
    snprintf(key, sizeof key, "svc %02X %s", id, name ? name : "?");
    count(key);
}

static void service_request(void* system, const struct cxx_string* service, const char* request,
                            const uint32_t* command, size_t words)
{
    (void)system;
    char key[128];
    snprintf(key, sizeof key, "ipc %.*s:%s %08X", (int)service->n, service->p, request ? request : "?",
             words ? command[0] : 0);
    count(key);
}

static void no_tick(void* system) { (void)system; }

/* the core's ModuleLoaded(Core::System&, const std::string& name, u32 address, u32 end): each CRO module the game loads,
   where (ldr:ro maps it there), appended to modules.txt as the game loads it */
static char modules_path[4096];
static void module_loaded(void* system, const struct cxx_string* name, uint32_t address, uint32_t end)
{
    (void)system;
    pthread_mutex_lock(&lock);
    FILE* f = fopen(modules_path, "a");
    if (f) {
        fprintf(f, "%.*s %08X %08X %llu\n", (int)name->n, name->p, address, end, total);
        fclose(f);
    }
    pthread_mutex_unlock(&lock);
}

static void stop(int sig)
{
    pthread_mutex_lock(&lock);
    write_counts();
    _exit(128 + sig);
}

static void at_exit(void)
{
    pthread_mutex_lock(&lock);
    write_counts();
    pthread_mutex_unlock(&lock);
}

/* writes "mov rax, to; jmp rax" over the start of from */
static int redirect(void* from, void* to)
{
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)from & ~(uintptr_t)(page - 1);
    if (mprotect((void*)start, 2 * page, PROT_READ | PROT_WRITE | PROT_EXEC))
        return 0;
    unsigned char code[12] = {0x48, 0xB8};
    uint64_t target = (uint64_t)(uintptr_t)to;
    memcpy(code + 2, &target, 8);
    code[10] = 0xFF;
    code[11] = 0xE0;
    memcpy(from, code, sizeof code);
    mprotect((void*)start, 2 * page, PROT_READ | PROT_EXEC);
    return 1;
}

void* dlopen(const char* file, int flags)
{
    static void* (*real)(const char*, int);
    if (!real)
        real = (void* (*)(const char*, int))dlsym(RTLD_NEXT, "dlopen");
    void* h = real(file, flags);
    const char* dir = getenv("PKM_TRACE_DIR");
    if (!h || !file || !dir || !strstr(file, "azahar_libretro"))
        return h;
    void (*configure)(int, const struct cxx_string*) = (void (*)(int, const struct cxx_string*))dlsym(
        h, "_ZN9Pomegrade9CodeTrace9ConfigureEbRKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
    void* kc = dlsym(h, "_ZN9Pomegrade9CodeTrace10KernelCallERN4Core6SystemEjPKc");
    void* sr = dlsym(h, "_ZN9Pomegrade9CodeTrace14ServiceRequestERN4Core6SystemERKNSt7__cxx1112basic_stringIcSt11"
                        "char_traitsIcESaIcEEEPKcPKjm");
    void* tick = dlsym(h, "_ZN9Pomegrade9CodeTrace4TickERN4Core6SystemE");
    void* ml = dlsym(h, "_ZN9Pomegrade9CodeTrace12ModuleLoadedERN4Core6SystemERKNSt7__cxx1112basic_stringIcSt11char_"
                        "traitsIcESaIcEEEjj");
    if (!configure || !kc || !sr || !tick || !ml) {
        fprintf(stderr, "azahar_trace_preload: the core has no Pomegrade code trace\n");
        return h;
    }
    if (!redirect(kc, (void*)kernel_call) || !redirect(sr, (void*)service_request) || !redirect(tick, (void*)no_tick) ||
        !redirect(ml, (void*)module_loaded)) {
        fprintf(stderr, "azahar_trace_preload: could not redirect the trace's hooks\n");
        return h;
    }
    snprintf(out_path, sizeof out_path, "%s/counts.txt", dir);
    snprintf(modules_path, sizeof modules_path, "%s/modules.txt", dir);
    atexit(at_exit);
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    struct cxx_string s; /* (the trace's own record still goes to dir, holding nothing: its hooks are ours) */
    s.n = strlen(dir);
    s.p = s.n < sizeof s.local ? s.local : strdup(dir);
    if (s.n >= sizeof s.local)
        s.capacity = s.n;
    memcpy(s.p, dir, s.n + 1);
    configure(1, &s);
    fprintf(stderr, "azahar_trace_preload: counting the game's calls into %s\n", out_path);
    return h;
}
