/* The CRO module loader (ldr:ro's work): a module's file, mapped at the address the game chose, rebased there and linked
 * with the static module (static.crs, the program's own table) and the modules already loaded, in the game's memory,
 * as the 3DS's loader leaves it. The steps and their rules are those of Azahar's ldr_ro (cro_helper.cpp: Rebase, Link,
 * Register, Fix; ldr_ro.cpp: LoadCRO, LoadCRS), checked against the modules Azahar loaded in a running game
 * (tools/cro_check.c: the same bytes).
 *
 * The header (after the 0x80-byte hash) is 0x4C words: magic, name offset, next / previous module (the list of loaded
 * modules, headed by the static module), file and bss sizes, the fixed size, then segment tags, then offset / size
 * pairs of the tables up to Fix0Barrier.
 */
#include "Cro.h"

#include <string.h>

#include "Memory.h"

enum {
    HASH = 0x80,
    F_MAGIC = 0, F_NAME = 1, F_NEXT = 2, F_PREVIOUS = 3, F_FILE_SIZE = 4, F_BSS_SIZE = 5, F_FIXED_SIZE = 6,
    F_ON_UNRESOLVED = 11, F_CODE = 12, F_CODE_SIZE = 13, F_DATA = 14, F_MODULE_NAME = 16, F_MODULE_NAME_SIZE = 17,
    F_SEGMENTS = 18, F_SEGMENT_NUM = 19, F_EXPORT_NAMED = 20, F_EXPORT_NAMED_NUM = 21, F_EXPORT_INDEXED = 22,
    F_EXPORT_INDEXED_NUM = 23, F_EXPORT_STRINGS = 24, F_EXPORT_STRINGS_SIZE = 25, F_EXPORT_TREE = 26,
    F_EXPORT_TREE_NUM = 27, F_IMPORT_MODULES = 28, F_IMPORT_MODULE_NUM = 29, F_EXTERNAL_RELOCATIONS = 30,
    F_EXTERNAL_RELOCATION_NUM = 31, F_IMPORT_NAMED = 32, F_IMPORT_NAMED_NUM = 33, F_IMPORT_INDEXED = 34,
    F_IMPORT_INDEXED_NUM = 35, F_IMPORT_ANONYMOUS = 36, F_IMPORT_ANONYMOUS_NUM = 37, F_IMPORT_STRINGS = 38,
    F_IMPORT_STRINGS_SIZE = 39, F_STATIC_ANONYMOUS = 40, F_STATIC_ANONYMOUS_NUM = 41, F_INTERNAL_RELOCATIONS = 42,
    F_INTERNAL_RELOCATION_NUM = 43, F_STATIC_RELOCATIONS = 44, F_STATIC_RELOCATION_NUM = 45, F_FIX0 = 46,
    F_FIX3 = F_EXPORT_NAMED, F_FIX2 = F_IMPORT_MODULES, F_FIX1 = F_STATIC_ANONYMOUS,
};
static const uint32_t MAGIC_CRO0 = 0x304F5243u, MAGIC_FIXD = 0x44584946u;
enum { SEGMENT_DATA = 2, SEGMENT_BSS = 3 };
enum { RELOCATION_NOTHING = 0, RELOCATION_ABSOLUTE = 2, RELOCATION_RELATIVE = 3, RELOCATION_ABSOLUTE2 = 38 };
/* each table's entry size, from the code to the static relocations (bytes for the code, data, names, strings) */
static const uint32_t ENTRY[17] = {1, 1, 1, 12, 8, 4, 1, 8, 20, 12, 8, 8, 8, 1, 8, 12, 12};

static uint32_t r32(uint32_t a) { uint32_t v; memcpy(&v, hos_pointer(a), 4); return v; }
static void w32(uint32_t a, uint32_t v) { memcpy(hos_pointer(a), &v, 4); }
static uint8_t r8(uint32_t a) { return *(uint8_t*)hos_pointer(a); }
static void w8(uint32_t a, uint8_t v) { *(uint8_t*)hos_pointer(a) = v; }

static uint32_t field(uint32_t m, int f) { return r32(m + HASH + 4u * (uint32_t)f); }
static void set_field(uint32_t m, int f, uint32_t v) { w32(m + HASH + 4u * (uint32_t)f, v); }
static uint32_t entry(uint32_t m, int table, uint32_t i, uint32_t size) { return field(m, table) + i * size; }

/* compares the C string at a (in the game's memory, at most max bytes) with s */
static int cstr_is(uint32_t a, uint32_t max, const char* s)
{
    for (uint32_t i = 0; i < max; i++) {
        uint8_t c = r8(a + i);
        if (c != (uint8_t)s[i])
            return 0;
        if (!c)
            return 1;
    }
    return 0;
}

static void cstr_get(uint32_t a, uint32_t max, char* out, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n && i < max; i++) {
        out[i] = (char)r8(a + (uint32_t)i);
        if (!out[i])
            return;
    }
    out[i] = 0;
}

/* a segment tag (segment index in the low 4 bits, the offset in the others) as an address; 0 when invalid */
static uint32_t tag_address(uint32_t m, uint32_t tag)
{
    uint32_t index = tag & 0xF, offset = tag >> 4;
    if (index >= field(m, F_SEGMENT_NUM))
        return 0;
    uint32_t e = entry(m, F_SEGMENTS, index, 12);
    if (offset >= r32(e + 4))
        return 0;
    return r32(e) + offset;
}

static int apply(uint32_t target, uint8_t type, uint32_t addend, uint32_t symbol, uint32_t future)
{
    switch (type) {
    case RELOCATION_NOTHING:
        return 0;
    case RELOCATION_ABSOLUTE:
    case RELOCATION_ABSOLUTE2:
        w32(target, symbol + addend);
        return 0;
    case RELOCATION_RELATIVE:
        w32(target, symbol + addend - future);
        return 0;
    default:
        /* the branch relocations (10, 28, 29, 42): neither game's modules have one (tools/cro_functions.py's
           tables: 0 branch words relocated in Omega Ruby's 145 modules); Azahar leaves them unimplemented too */
        return -1;
    }
}

/* a batch of relocations (12-byte entries until the one marked last) applied with symbol; its first entry then marks
   it resolved (or not, for a reset) */
static int apply_batch(uint32_t m, uint32_t batch, uint32_t symbol, int reset)
{
    if (symbol == 0 && !reset)
        return -1;
    for (uint32_t at = batch;; at += 12) {
        uint32_t target = tag_address(m, r32(at));
        if (!target || apply(target, r8(at + 4), r32(at + 8), symbol, target))
            return -1;
        if (r8(at + 5))
            break;
    }
    w8(batch + 6, reset ? 0 : 1);
    return 0;
}

uint32_t hos_cro_find_export(uint32_t m, const char* name)
{
    if (!field(m, F_EXPORT_TREE_NUM))
        return 0;
    size_t len = strlen(name);
    uint32_t tree = field(m, F_EXPORT_TREE);
    uint16_t next;
    memcpy(&next, hos_pointer(tree + 2), 2); /* the root's left child */
    uint32_t found;
    for (;;) {
        uint32_t e = tree + 8u * (next & 0x7FFFu);
        if (next & 0x8000u) {
            uint16_t index;
            memcpy(&index, hos_pointer(e + 6), 2);
            found = index;
            break;
        }
        uint16_t test_bit, left, right;
        memcpy(&test_bit, hos_pointer(e), 2);
        memcpy(&left, hos_pointer(e + 2), 2);
        memcpy(&right, hos_pointer(e + 4), 2);
        uint16_t byte = (uint16_t)(test_bit >> 3), bit = (uint16_t)(test_bit & 7);
        next = byte >= len ? left : ((uint8_t)name[byte] >> bit) & 1 ? right : left;
    }
    if (found >= field(m, F_EXPORT_NAMED_NUM))
        return 0;
    uint32_t e = entry(m, F_EXPORT_NAMED, found, 8);
    if (!cstr_is(r32(e), field(m, F_EXPORT_STRINGS_SIZE), name))
        return 0;
    return tag_address(m, r32(e + 4));
}

static int rebase_header(uint32_t m, uint32_t size)
{
    if (field(m, F_MAGIC) != MAGIC_CRO0 || field(m, F_NEXT) || field(m, F_PREVIOUS) || field(m, F_FIXED_SIZE))
        return -1;
    if (field(m, F_NAME))
        set_field(m, F_NAME, field(m, F_NAME) + m);
    for (int f = F_CODE; f < F_FIX0; f += 2)
        if (field(m, f))
            set_field(m, f, field(m, f) + m);
    for (int f = F_CODE, i = 0; f < F_FIX0; f += 2, i++)
        if (field(m, f) + field(m, f + 1) * ENTRY[i] > m + size)
            return -1;
    return 0;
}

/* the segments where they are: the data and bss segments at the game's buffers, the others in the module; the data
   segment's old place (in the module) returned */
static uint32_t rebase_segments(uint32_t m, uint32_t data, uint32_t data_size, uint32_t bss, uint32_t bss_size)
{
    uint32_t old_data = 0;
    for (uint32_t i = 0; i < field(m, F_SEGMENT_NUM); i++) {
        uint32_t e = entry(m, F_SEGMENTS, i, 12);
        uint32_t off = r32(e), size = r32(e + 4), type = r32(e + 8);
        if (type == SEGMENT_DATA) {
            if (size) {
                if (size > data_size)
                    return 0xFFFFFFFFu;
                old_data = off;
                w32(e, data);
            }
        } else if (type == SEGMENT_BSS) {
            if (size) {
                if (size > bss_size)
                    return 0xFFFFFFFFu;
                w32(e, bss);
            }
        } else if (off) {
            w32(e, off + m);
        }
    }
    return old_data + m;
}

static void rebase_offsets(uint32_t m)
{
    for (uint32_t i = 0; i < field(m, F_EXPORT_NAMED_NUM); i++) {
        uint32_t e = entry(m, F_EXPORT_NAMED, i, 8);
        if (r32(e))
            w32(e, r32(e) + m);
    }
    for (uint32_t i = 0; i < field(m, F_IMPORT_MODULE_NUM); i++) {
        uint32_t e = entry(m, F_IMPORT_MODULES, i, 20);
        static const uint32_t offsets[3] = {0, 4, 12}; /* its name, indexed table and anonymous table */
        for (int k = 0; k < 3; k++)
            if (r32(e + offsets[k]))
                w32(e + offsets[k], r32(e + offsets[k]) + m);
    }
}

static void rebase_import_batches(uint32_t m)
{
    for (uint32_t i = 0; i < field(m, F_IMPORT_NAMED_NUM); i++) {
        uint32_t e = entry(m, F_IMPORT_NAMED, i, 8);
        if (r32(e))
            w32(e, r32(e) + m);
        if (r32(e + 4))
            w32(e + 4, r32(e + 4) + m);
    }
    for (uint32_t i = 0; i < field(m, F_IMPORT_INDEXED_NUM); i++) {
        uint32_t e = entry(m, F_IMPORT_INDEXED, i, 8);
        if (r32(e + 4))
            w32(e + 4, r32(e + 4) + m);
    }
    for (uint32_t i = 0; i < field(m, F_IMPORT_ANONYMOUS_NUM); i++) {
        uint32_t e = entry(m, F_IMPORT_ANONYMOUS, i, 8);
        if (r32(e + 4))
            w32(e + 4, r32(e + 4) + m);
    }
}

/* every external relocation pointed at the module's on-unresolved function, every batch marked unresolved */
static int reset_external(uint32_t m)
{
    uint32_t unresolved = tag_address(m, field(m, F_ON_UNRESOLVED));
    uint32_t n = field(m, F_EXTERNAL_RELOCATION_NUM);
    if (n && !r8(entry(m, F_EXTERNAL_RELOCATIONS, n - 1, 12) + 5))
        return -1;
    int begin = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = entry(m, F_EXTERNAL_RELOCATIONS, i, 12);
        uint32_t target = tag_address(m, r32(e));
        if (!target || apply(target, r8(e + 4), r32(e + 8), unresolved, target))
            return -1;
        if (begin)
            w8(e + 6, 0);
        begin = r8(e + 5) != 0;
    }
    return 0;
}

static int static_anonymous_to_crs(uint32_t m, uint32_t crs)
{
    for (uint32_t i = 0; i < field(m, F_STATIC_ANONYMOUS_NUM); i++) {
        uint32_t e = entry(m, F_STATIC_ANONYMOUS, i, 8);
        uint32_t batch = r32(e + 4) + m;
        if (apply_batch(crs, batch, tag_address(m, r32(e)), 0))
            return -1;
    }
    return 0;
}

static int internal_relocations(uint32_t m, uint32_t old_data)
{
    uint32_t segments = field(m, F_SEGMENT_NUM);
    for (uint32_t i = 0; i < field(m, F_INTERNAL_RELOCATION_NUM); i++) {
        uint32_t e = entry(m, F_INTERNAL_RELOCATIONS, i, 12);
        uint32_t tag = r32(e), at = tag_address(m, tag);
        uint8_t type = r8(e + 4), symbol_segment = r8(e + 5);
        if (!at || symbol_segment >= segments)
            return -1;
        /* a target in the data segment is written in the module (where the data still is: the game copies it to its
           buffer after loading), at the address it will have */
        uint32_t target = r32(entry(m, F_SEGMENTS, tag & 0xF, 12) + 8) == SEGMENT_DATA ? old_data + (tag >> 4) : at;
        if (apply(target, type, r32(e + 8), r32(entry(m, F_SEGMENTS, symbol_segment, 12)), at))
            return -1;
    }
    return 0;
}

/* the modules list: from the static module, its next field heads the auto-linked ones */
static int import_named(uint32_t m, uint32_t crs, int exit_only)
{
    uint32_t strings = field(m, F_IMPORT_STRINGS_SIZE);
    for (uint32_t i = 0; i < field(m, F_IMPORT_NAMED_NUM); i++) {
        uint32_t e = entry(m, F_IMPORT_NAMED, i, 8);
        uint32_t batch = r32(e + 4);
        if (exit_only) {
            if (!cstr_is(r32(e), strings, "__aeabi_atexit"))
                continue;
        } else if (r8(batch + 6)) {
            continue; /* (resolved) */
        }
        char name[256];
        cstr_get(r32(e), strings, name, sizeof name);
        for (uint32_t s = crs; s; s = field(s, F_NEXT)) {
            uint32_t symbol = hos_cro_find_export(s, exit_only ? "nnroAeabiAtexit_" : name);
            if (symbol) {
                if (apply_batch(m, batch, symbol, 0))
                    return -1;
                break;
            }
        }
    }
    return 0;
}

static int module_imports(uint32_t m, uint32_t crs)
{
    uint32_t strings = field(m, F_IMPORT_STRINGS_SIZE);
    for (uint32_t i = 0; i < field(m, F_IMPORT_MODULE_NUM); i++) {
        uint32_t e = entry(m, F_IMPORT_MODULES, i, 20);
        for (uint32_t s = crs; s; s = field(s, F_NEXT)) {
            char want[64];
            cstr_get(r32(e), strings, want, sizeof want);
            if (!cstr_is(field(s, F_MODULE_NAME), 0xFFFF, want))
                continue;
            for (uint32_t j = 0; j < r32(e + 8); j++) {
                uint32_t im = r32(e + 4) + 8 * j;
                uint32_t ex = entry(s, F_EXPORT_INDEXED, r32(im), 4);
                if (apply_batch(m, r32(im + 4), tag_address(s, r32(ex)), 0))
                    return -1;
            }
            for (uint32_t j = 0; j < r32(e + 16); j++) {
                uint32_t im = r32(e + 12) + 8 * j;
                if (apply_batch(m, r32(im + 4), tag_address(s, r32(im)), 0))
                    return -1;
            }
            break;
        }
    }
    return 0;
}

/* this module's exports given to a module loaded before it (target) */
static int export_to(uint32_t m, uint32_t target)
{
    uint32_t strings = field(target, F_IMPORT_STRINGS_SIZE);
    for (uint32_t i = 0; i < field(target, F_IMPORT_NAMED_NUM); i++) {
        uint32_t e = entry(target, F_IMPORT_NAMED, i, 8);
        uint32_t batch = r32(e + 4);
        if (r8(batch + 6))
            continue;
        char name[256];
        cstr_get(r32(e), strings, name, sizeof name);
        uint32_t symbol = hos_cro_find_export(m, name);
        if (symbol && apply_batch(target, batch, symbol, 0))
            return -1;
    }
    for (uint32_t i = 0; i < field(target, F_IMPORT_MODULE_NUM); i++) {
        uint32_t e = entry(target, F_IMPORT_MODULES, i, 20);
        char want[64];
        cstr_get(r32(e), strings, want, sizeof want);
        if (!cstr_is(field(m, F_MODULE_NAME), 0xFFFF, want))
            continue;
        for (uint32_t j = 0; j < r32(e + 8); j++) {
            uint32_t im = r32(e + 4) + 8 * j;
            uint32_t ex = entry(m, F_EXPORT_INDEXED, r32(im), 4);
            if (apply_batch(target, r32(im + 4), tag_address(m, r32(ex)), 0))
                return -1;
        }
        for (uint32_t j = 0; j < r32(e + 16); j++) {
            uint32_t im = r32(e + 12) + 8 * j;
            if (apply_batch(target, r32(im + 4), tag_address(m, r32(im)), 0))
                return -1;
        }
    }
    return 0;
}

int hos_cro_load_crs(uint32_t crs, uint32_t size)
{
    set_field(crs, F_NEXT, 0);
    set_field(crs, F_PREVIOUS, 0);
    if (rebase_header(crs, size))
        return -1;
    rebase_offsets(crs);
    if (reset_external(crs))
        return -1;
    rebase_import_batches(crs);
    return internal_relocations(crs, 0);
}

uint32_t hos_cro_load(uint32_t m, uint32_t size, uint32_t data, uint32_t data_size, uint32_t bss, uint32_t bss_size,
                      int auto_link, uint32_t fix_level, uint32_t crs, int link_on_load_bug_fix)
{
    if (rebase_header(m, size))
        return 0;
    uint32_t old_data = rebase_segments(m, data, data_size, bss, bss_size);
    if (old_data == 0xFFFFFFFFu)
        return 0;
    rebase_offsets(m);
    if (reset_external(m))
        return 0;
    rebase_import_batches(m);
    if (static_anonymous_to_crs(m, crs) || internal_relocations(m, old_data) || import_named(m, crs, 1))
        return 0;
    /* link: its imports from the modules loaded (the data segment's entry pointed back into the module meanwhile, for
       the newer LoadCRO: link_on_load_bug_fix), then its exports to them */
    uint32_t saved = 0, data_entry = field(m, F_SEGMENT_NUM) >= 2 ? entry(m, F_SEGMENTS, 2, 12) : 0;
    if (link_on_load_bug_fix && data_entry) {
        saved = r32(data_entry);
        w32(data_entry, field(m, F_DATA));
    }
    int bad = import_named(m, crs, 0) || module_imports(m, crs);
    if (link_on_load_bug_fix && data_entry)
        w32(data_entry, saved);
    if (bad)
        return 0;
    for (uint32_t t = crs; t; t = field(t, F_NEXT))
        if (export_to(m, t))
            return 0;
    /* register: at the end of the list the static module heads (auto-linked ones from its next field) */
    uint32_t head = auto_link ? field(crs, F_NEXT) : field(crs, F_PREVIOUS);
    if (head) {
        uint32_t tail = field(head, F_PREVIOUS);
        set_field(m, F_PREVIOUS, tail);
        set_field(tail, F_NEXT, m);
        set_field(head, F_PREVIOUS, m);
    } else {
        set_field(m, F_PREVIOUS, m);
        set_field(crs, auto_link ? F_NEXT : F_PREVIOUS, m);
    }
    set_field(m, F_NEXT, 0);
    /* fix: the tables past the level's barrier dropped, the module's size cut there (page-aligned) */
    static const int barrier[4] = {F_FIX0, F_FIX1, F_FIX2, F_FIX3};
    uint32_t end = m + HASH + 4u * F_FIX0;
    if (field(m, F_CODE) + field(m, F_CODE_SIZE) > end)
        end = field(m, F_CODE) + field(m, F_CODE_SIZE);
    for (int f = F_MODULE_NAME, i = 2; f != barrier[fix_level & 3]; f += 2, i++)
        if (field(m, f) + field(m, f + 1) * ENTRY[i] > end)
            end = field(m, f) + field(m, f + 1) * ENTRY[i];
    if (fix_level) {
        set_field(m, F_MAGIC, MAGIC_FIXD);
        for (int f = barrier[fix_level & 3]; f < F_FIX0; f += 2) {
            set_field(m, f, end);
            set_field(m, f + 1, 0);
        }
    }
    end = (end + 0xFFFu) & ~0xFFFu;
    set_field(m, F_FIXED_SIZE, end - m);
    return end - m;
}

/* unloading (ldr_ro.cpp UnloadCRO; cro_helper.cpp Unregister, Unlink, ClearRelocations, Unrebase) */

static void unregister(uint32_t m, uint32_t crs)
{
    uint32_t next_head = field(crs, F_NEXT), previous_head = field(crs, F_PREVIOUS);
    uint32_t next = field(m, F_NEXT), previous = field(m, F_PREVIOUS);
    if (m == next_head || m == previous_head) { /* the head removed: the next one heads, pointing at the tail */
        if (next)
            set_field(next, F_PREVIOUS, previous);
        set_field(crs, m == previous_head ? F_PREVIOUS : F_NEXT, next);
    } else if (next) {
        set_field(previous, F_NEXT, next);
        set_field(next, F_PREVIOUS, previous);
    } else { /* the tail removed */
        set_field(previous, F_NEXT, 0);
        if (next_head && field(next_head, F_PREVIOUS) == m)
            set_field(next_head, F_PREVIOUS, previous);
        else if (previous_head && field(previous_head, F_PREVIOUS) == m)
            set_field(previous_head, F_PREVIOUS, previous);
    }
    set_field(m, F_NEXT, 0);
    set_field(m, F_PREVIOUS, 0);
}

/* the module's own imports pointed back at its on-unresolved function */
static int reset_imports(uint32_t m)
{
    uint32_t unresolved = tag_address(m, field(m, F_ON_UNRESOLVED));
    static const int tables[3][2] = {{F_IMPORT_NAMED, F_IMPORT_NAMED_NUM}, {F_IMPORT_INDEXED, F_IMPORT_INDEXED_NUM},
                                     {F_IMPORT_ANONYMOUS, F_IMPORT_ANONYMOUS_NUM}};
    for (int t = 0; t < 3; t++)
        for (uint32_t i = 0; i < field(m, tables[t][1]); i++)
            if (apply_batch(m, r32(entry(m, tables[t][0], i, 8) + 4), unresolved, 1))
                return -1;
    return 0;
}

/* what a loaded module (target) imported from this one, pointed back at target's on-unresolved function */
static int unexport_from(uint32_t m, uint32_t target)
{
    uint32_t unresolved = tag_address(target, field(target, F_ON_UNRESOLVED));
    uint32_t strings = field(target, F_IMPORT_STRINGS_SIZE);
    for (uint32_t i = 0; i < field(target, F_IMPORT_NAMED_NUM); i++) {
        uint32_t e = entry(target, F_IMPORT_NAMED, i, 8);
        uint32_t batch = r32(e + 4);
        if (!r8(batch + 6))
            continue;
        char name[256];
        cstr_get(r32(e), strings, name, sizeof name);
        if (hos_cro_find_export(m, name) && apply_batch(target, batch, unresolved, 1))
            return -1;
    }
    for (uint32_t i = 0; i < field(target, F_IMPORT_MODULE_NUM); i++) {
        uint32_t e = entry(target, F_IMPORT_MODULES, i, 20);
        char want[64];
        cstr_get(r32(e), strings, want, sizeof want);
        if (!cstr_is(field(m, F_MODULE_NAME), 0xFFFF, want))
            continue;
        for (uint32_t j = 0; j < r32(e + 8); j++)
            if (apply_batch(target, r32(r32(e + 4) + 8 * j + 4), unresolved, 1))
                return -1;
        for (uint32_t j = 0; j < r32(e + 16); j++)
            if (apply_batch(target, r32(r32(e + 12) + 8 * j + 4), unresolved, 1))
                return -1;
    }
    return 0;
}

static int clear(uint32_t target, uint8_t type)
{
    switch (type) {
    case RELOCATION_NOTHING:
        return 0;
    case RELOCATION_ABSOLUTE:
    case RELOCATION_ABSOLUTE2:
    case RELOCATION_RELATIVE:
        w32(target, 0);
        return 0;
    default:
        return -1; /* (Azahar: unimplemented, as for applying them) */
    }
}

static int clear_relocations(uint32_t m)
{
    int begin = 1;
    for (uint32_t i = 0; i < field(m, F_EXTERNAL_RELOCATION_NUM); i++) {
        uint32_t e = entry(m, F_EXTERNAL_RELOCATIONS, i, 12), target = tag_address(m, r32(e));
        if (!target || clear(target, r8(e + 4)))
            return -1;
        if (begin)
            w8(e + 6, 0);
        begin = r8(e + 5) != 0;
    }
    for (uint32_t i = 0; i < field(m, F_INTERNAL_RELOCATION_NUM); i++) {
        uint32_t e = entry(m, F_INTERNAL_RELOCATIONS, i, 12), target = tag_address(m, r32(e));
        if (!target || clear(target, r8(e + 4)))
            return -1;
    }
    return 0;
}

static void unrebase(uint32_t m)
{
    for (uint32_t i = 0; i < field(m, F_IMPORT_ANONYMOUS_NUM); i++) {
        uint32_t e = entry(m, F_IMPORT_ANONYMOUS, i, 8);
        if (r32(e + 4))
            w32(e + 4, r32(e + 4) - m);
    }
    for (uint32_t i = 0; i < field(m, F_IMPORT_INDEXED_NUM); i++) {
        uint32_t e = entry(m, F_IMPORT_INDEXED, i, 8);
        if (r32(e + 4))
            w32(e + 4, r32(e + 4) - m);
    }
    for (uint32_t i = 0; i < field(m, F_IMPORT_NAMED_NUM); i++) {
        uint32_t e = entry(m, F_IMPORT_NAMED, i, 8);
        if (r32(e))
            w32(e, r32(e) - m);
        if (r32(e + 4))
            w32(e + 4, r32(e + 4) - m);
    }
    for (uint32_t i = 0; i < field(m, F_IMPORT_MODULE_NUM); i++) {
        uint32_t e = entry(m, F_IMPORT_MODULES, i, 20);
        static const uint32_t offsets[3] = {0, 4, 12};
        for (int k = 0; k < 3; k++)
            if (r32(e + offsets[k]))
                w32(e + offsets[k], r32(e + offsets[k]) - m);
    }
    for (uint32_t i = 0; i < field(m, F_EXPORT_NAMED_NUM); i++) {
        uint32_t e = entry(m, F_EXPORT_NAMED, i, 8);
        if (r32(e))
            w32(e, r32(e) - m);
    }
    for (uint32_t i = 0; i < field(m, F_SEGMENT_NUM); i++) {
        uint32_t e = entry(m, F_SEGMENTS, i, 12);
        if (r32(e + 8) == SEGMENT_BSS)
            w32(e, 0);
        else if (r32(e))
            w32(e, r32(e) - m);
    }
    set_field(m, F_NEXT, 0);
    set_field(m, F_PREVIOUS, 0);
    set_field(m, F_FIXED_SIZE, 0);
    if (field(m, F_NAME))
        set_field(m, F_NAME, field(m, F_NAME) - m);
    for (int f = F_CODE; f < F_FIX0; f += 2)
        if (field(m, f))
            set_field(m, f, field(m, f) - m);
}

int hos_cro_unload(uint32_t m, uint32_t crs)
{
    uint32_t magic = field(m, F_MAGIC);
    if (magic != MAGIC_CRO0 && magic != MAGIC_FIXD)
        return -1;
    unregister(m, crs);
    if (reset_imports(m))
        return -1;
    for (uint32_t t = field(crs, F_NEXT); t; t = field(t, F_NEXT)) /* (the auto-linked ones, as the RO service) */
        if (unexport_from(m, t))
            return -1;
    if (magic != MAGIC_FIXD && clear_relocations(m))
        return -1;
    unrebase(m);
    return 0;
}

uint32_t hos_cro_fixed_size(uint32_t m)
{
    return field(m, F_FIXED_SIZE);
}

void hos_cro_name(uint32_t m, char* out, size_t n)
{
    cstr_get(field(m, F_MODULE_NAME), field(m, F_MODULE_NAME_SIZE), out, n);
}
