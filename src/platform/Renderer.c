/* The renderer's entry: see Renderer.h. Register meanings as 3dbrew's GPU/Internal Registers and Azahar's regs_*.h. */
#include "Renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hos/Memory.h"
#include "PicaFragment.h"
#include "PicaRegs.h"
#include "PicaVertex.h"
#include "VulkanRenderer.h"
#include <math.h>

extern uint8_t* hos_memory;

static PicaState state;
static int ready;
static FILE* stats;

/* the GPU's physical addresses in the game's view: FCRAM 0x20000000 at the linear heap's 0x14000000, VRAM 0x18000000
   at 0x1F000000 (src/hos/Loader.c maps both) */
static const uint32_t* physical(uint32_t address, uint32_t bytes, void* user)
{
    (void)user;
    if (address >= 0x20000000u && address + bytes <= 0x28000000u)
        return (const uint32_t*)(hos_memory + address - 0x0C000000u);
    if (address >= 0x18000000u && address + bytes <= 0x18600000u)
        return (const uint32_t*)(hos_memory + address + 0x07000000u);
    return NULL;
}

/* the census: each feature's values and how many draws use each */
enum { KEYS = 4096 };
static struct { char key[96]; unsigned long long draws; } census[KEYS];
static unsigned long long draws, triangles, finite_vertices, inside_vertices, all_vertices;

static void count(const char* fmt, ...)
    __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void count(const char* fmt, ...)
{
    char key[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(key, sizeof key, fmt, ap);
    va_end(ap);
    for (int k = 0; k < KEYS; k++) {
        if (!census[k].key[0]) {
            snprintf(census[k].key, sizeof census[k].key, "%s", key);
            census[k].draws = 1;
            return;
        }
        if (!strcmp(census[k].key, key)) {
            census[k].draws++;
            return;
        }
    }
}

/* $HOS_FRAGMENT_DIR: each distinct fragment shader the draws need, one file each (for the compile check) */
static void save_fragment(const PicaState* s)
{
    static const char* dir;
    static int checked, saved;
    static uint64_t seen[4096];
    if (!checked) { dir = getenv("HOS_FRAGMENT_DIR"); checked = 1; }
    if (!dir || saved >= 4096)
        return;
    static char glsl[1 << 17];
    long n = pica_fragment_glsl(s, glsl, sizeof glsl);
    if (n < 0)
        return;
    uint64_t h = 1469598103934665603ull;
    for (long k = 0; k < n; k++)
        h = (h ^ (uint8_t)glsl[k]) * 1099511628211ull;
    for (int k = 0; k < saved; k++)
        if (seen[k] == h)
            return;
    seen[saved] = h;
    char path[600];
    snprintf(path, sizeof path, "%s/fragment_%04d.frag", dir, saved++);
    FILE* f = fopen(path, "w");
    if (f) { fwrite(glsl, 1, n, f); fclose(f); }
}

static void on_draw(PicaState* s, int indexed, void* user)
{
    (void)user;
    save_fragment(s);
    if (!vk_renderer_init()) { /* drawing ($HOS_RENDER_DIR): the census skipped */
        vk_renderer_draw(s, indexed);
        return;
    }
    static PicaVertex verts[1 << 16];
    const long n = pica_vertex_draw(s, indexed, verts, 1 << 16);
    if (n < 0)
        count("vertices: memory not reachable");
    else {
        long finite = 0, inside = 0;
        for (long k = 0; k < n; k++) {
            const float* p = verts[k].position;
            if (isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]) && isfinite(p[3]) && p[3] != 0) {
                finite++;
                if (fabsf(p[0] / p[3]) <= 1.01f && fabsf(p[1] / p[3]) <= 1.01f)
                    inside++;
            }
        }
        triangles += n / 3;
        finite_vertices += finite;
        inside_vertices += inside;
        all_vertices += n;
    }
    const uint32_t* r = s->reg;
    draws++;
    static const char* topology[] = {"triangles", "strip", "fan", "geometry shader"};
    count("primitive %s%s", topology[r[0x25E] >> 8 & 3], indexed ? ", indexed" : "");
    count("geometry shader %s", (r[0x229] & 3) ? "on" : "off");
    const uint32_t units = r[0x080];
    count("texture units on: %s%s%s%s", units & 1 ? "0 " : "", units & 2 ? "1 " : "", units & 4 ? "2 " : "",
          units & 8 ? "3 (procedural)" : "");
    static const uint32_t type_reg[3] = {0x08E, 0x096, 0x09E};
    for (int u = 0; u < 3; u++)
        if (units >> u & 1)
            count("texture format %u", r[type_reg[u]] & 0xF);
    static const uint32_t stage_reg[6] = {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8};
    for (int k = 0; k < 6; k++) {
        const uint32_t combine = r[stage_reg[k] + 2];
        count("combiner op rgb %u alpha %u", combine & 0xF, combine >> 16 & 0xF);
        const uint32_t src = r[stage_reg[k]];
        for (int i = 0; i < 3; i++)
            count("combiner source %u", src >> (4 * i) & 0xF);
    }
    count("fragment lighting %s", (r[0x08F] & 1) ? "on" : "off");
    count("fog mode %u", r[0x0E0] & 7);
    count("alpha test %s", (r[0x104] & 1) ? "on" : "off");
    count("blending %s", (r[0x100] >> 8 & 1) ? "on" : "off (logic op)");
    count("depth test %s, write %s", (r[0x107] & 1) ? "on" : "off", (r[0x107] >> 12 & 1) ? "on" : "off");
    count("stencil %s", (r[0x105] & 1) ? "on" : "off");
    count("color buffer %08X format %u, %ux%u", (r[0x11D] & 0x0FFFFFFF) << 3, r[0x117] >> 16 & 7, r[0x11E] & 0x7FF,
          (r[0x11E] >> 12 & 0x3FF) + 1);
    count("depth buffer format %u", r[0x116] & 3);
}

void renderer_submit_list(uint32_t address, uint32_t size)
{
    if (!ready) {
        const char* path = getenv("HOS_DRAW_STATS");
        if (!path && !getenv("HOS_FRAGMENT_DIR") && !getenv("HOS_RENDER_DIR"))
            return;
        stats = path ? fopen(path, "w") : NULL;
        pica_state_init(&state, on_draw, physical, NULL);
        ready = 1;
    }
    pica_state_run(&state, (const uint32_t*)(hos_memory + address), size / 4);
}

void renderer_report(void)
{
    if (!stats)
        return;
    fprintf(stats, "# draws %llu, triangles %llu; vertices %llu, of them finite %llu, inside the screen %llu\n"
                   "# feature\tdraws\n", draws, triangles, all_vertices, finite_vertices, inside_vertices);
    for (int k = 0; k < KEYS && census[k].key[0]; k++)
        fprintf(stats, "%s\t%llu\n", census[k].key, census[k].draws);
    fclose(stats);
}

void renderer_display(uint32_t source)
{
    /* the game's virtual address as the GPU's physical one: VRAM 0x1F000000 -> 0x18000000, linear heap 0x14000000 ->
       FCRAM 0x20000000 */
    uint32_t physical_address = source;
    if (source >= 0x1F000000u && source < 0x1F600000u)
        physical_address = source - 0x07000000u;
    else if (source >= 0x14000000u && source < 0x1C000000u)
        physical_address = source + 0x0C000000u;
    vk_renderer_display(physical_address);
}
