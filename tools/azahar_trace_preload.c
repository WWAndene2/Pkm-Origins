/* Turns on Pomegrade's code trace (azahar/src/core/pomegrade_code_trace.h: every system call and service request the
   game makes, its files and modules) in a headless run, which has no setting for it: loaded before Pomegrade's
   retro_host (LD_PRELOAD), it waits for the Azahar core to be loaded and calls its Configure before the game starts.
   Nothing in Pomegrade is changed. The record goes to $PKM_TRACE_DIR/<program id>_<date>/ (calls.txt: one line per
   call, svc or ipc with the service and request names; summary.txt: the total, to tell whether the ring of 131,072
   kept them all), written every 30 s and when the game stops. Read by tools/hos_calls.py.

     cc -shared -fPIC -O1 -o azahar_trace_preload.so tools/azahar_trace_preload.c -ldl
     PKM_TRACE_DIR=<folder> LD_PRELOAD=<that .so> <Pomegrade's run_local.sh ...>
*/
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pomegrade::CodeTrace::Configure(bool, const std::string&) takes a C++ string: made here in the layout of libstdc++'s
   std::string of the C++11 ABI, which the Linux build of the core uses (pointer, size, then a 16-byte local buffer or
   the capacity). */
struct cxx_string {
    char* p;
    size_t n;
    union {
        char local[16];
        size_t capacity;
    };
};

void* dlopen(const char* file, int flags)
{
    static void* (*real)(const char*, int);
    if (!real)
        real = (void* (*)(const char*, int))dlsym(RTLD_NEXT, "dlopen");
    void* h = real(file, flags);
    const char* dir = getenv("PKM_TRACE_DIR");
    if (h && file && dir && strstr(file, "azahar_libretro")) {
        void (*configure)(int, const struct cxx_string*) = (void (*)(int, const struct cxx_string*))dlsym(
            h, "_ZN9Pomegrade9CodeTrace9ConfigureEbRKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
        if (!configure) {
            fprintf(stderr, "azahar_trace_preload: the core has no Pomegrade code trace\n");
            return h;
        }
        struct cxx_string s;
        s.n = strlen(dir);
        if (s.n < sizeof s.local) {
            s.p = s.local;
        } else {
            s.p = strdup(dir); /* (kept: the core copies it) */
            s.capacity = s.n;
        }
        memcpy(s.p, dir, s.n + 1);
        configure(1, &s);
        fprintf(stderr, "azahar_trace_preload: code trace on, to %s\n", dir);
    }
    return h;
}
