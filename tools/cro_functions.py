#!/usr/bin/env python3
"""The function list of the CRO modules (step 4 of docs/RECOMPILATION_PLAN.md §9), as function_list.py does for the .code:
every module's code segment split into functions, so the shared count covers the modules too.

  cro_functions.py <cro dir> <out.tsv>

A CRO's layout is Azahar's loader's (azahar/src/core/hle/service/ldr_ro/cro_helper.h), read the way Pomegrade's
tools/remake/prototype/cro_dis.py reads it: a 0x80-byte hash, the header's u32 fields, a segment table (offset, size,
type; offsets are file offsets in the file as stored). Starts: the targets of bl / blx inside the code segment, the
symbols the module exports into it, then function_list.py's flow pass over the segment.

out.tsv (names and offsets only, no game data): module, offset in the file, how it was found (call / export / pointer / flow), its
instruction set (arm / thumb), its size in bytes (as function_list.py gives it).
`load()` also gives each module's relocated words (the words the loader rewrites: imports and internal relocations),
which shared_functions.py sets aside as it does addresses in the .code.
Needs capstone (imported by function_list.py).
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from function_list import flow_pass  # noqa: E402

HASH = 0x80
FIELDS = ["Magic", "NameOffset", "NextCRO", "PreviousCRO", "FileSize", "BssSize", "FixedSize", "UnknownZero",
          "UnkSegmentTag", "OnLoadSegmentTag", "OnExitSegmentTag", "OnUnresolvedSegmentTag",
          "CodeOffset", "CodeSize", "DataOffset", "DataSize", "ModuleNameOffset", "ModuleNameSize",
          "SegmentTableOffset", "SegmentNum",
          "ExportNamedSymbolTableOffset", "ExportNamedSymbolNum", "ExportIndexedSymbolTableOffset", "ExportIndexedSymbolNum",
          "ExportStringsOffset", "ExportStringsSize", "ExportTreeTableOffset", "ExportTreeNum",
          "ImportModuleTableOffset", "ImportModuleNum", "ExternalRelocationTableOffset", "ExternalRelocationNum",
          "ImportNamedSymbolTableOffset", "ImportNamedSymbolNum", "ImportIndexedSymbolTableOffset", "ImportIndexedSymbolNum",
          "ImportAnonymousSymbolTableOffset", "ImportAnonymousSymbolNum", "ImportStringsOffset", "ImportStringsSize",
          "StaticAnonymousSymbolTableOffset", "StaticAnonymousSymbolNum", "InternalRelocationTableOffset",
          "InternalRelocationNum", "StaticRelocationTableOffset", "StaticRelocationNum"]


class Module:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        if self.data[HASH:HASH + 4] != b"CRO0":
            raise ValueError("not a CRO")
        self.h = {n: struct.unpack_from("<I", self.data, HASH + 4 * i)[0] for i, n in enumerate(FIELDS)}
        # the name in its header, the one imports and the loader use (the file DllEshop.cro is the module DllEShop)
        at = self.h["ModuleNameOffset"]
        self.name = self.data[at:self.data.index(0, at)].decode("ascii")
        self.segments = [struct.unpack_from("<III", self.data, self.h["SegmentTableOffset"] + 12 * i)
                         for i in range(self.h["SegmentNum"])]
        code = [s for s in self.segments if s[2] == 0]
        self.code_start, self.code_end = (code[0][0], code[0][0] + code[0][1]) if code else (0, 0)
        # every word the loader rewrites: external (imports) and internal relocation tables, 12-byte entries whose first
        # word is the patched place's segment tag
        self.relocated = set()
        for table, num in (("ExternalRelocationTableOffset", "ExternalRelocationNum"),
                           ("InternalRelocationTableOffset", "InternalRelocationNum")):
            for i in range(self.h[num]):
                t = self.tag(struct.unpack_from("<I", self.data, self.h[table] + 12 * i)[0])
                if t is not None:
                    self.relocated.add(t)

    def tag(self, raw):
        index, offset = raw & 0xF, raw >> 4
        if index >= len(self.segments) or offset >= self.segments[index][1]:
            return None
        return self.segments[index][0] + offset

    def cstr(self, at):
        return self.data[at:self.data.index(0, at)].decode("ascii", "replace")

    def batch(self, at):
        """the places one import's relocation batch patches (entries until the end flag)"""
        out = []
        while True:
            tag, _kind, end = struct.unpack_from("<IBB", self.data, at)
            t = self.tag(tag)
            if t is not None:
                out.append(t)
            if end:
                return out
            at += 12

    def named_exports(self):
        """{name: place in this module}"""
        out = {}
        for i in range(self.h["ExportNamedSymbolNum"]):
            name_off, tag = struct.unpack_from("<II", self.data, self.h["ExportNamedSymbolTableOffset"] + 8 * i)
            t = self.tag(tag)
            if t is not None:
                out[self.cstr(name_off)] = t
        return out

    def indexed_export(self, index):
        if index >= self.h["ExportIndexedSymbolNum"]:
            return None
        return self.tag(struct.unpack_from("<I", self.data, self.h["ExportIndexedSymbolTableOffset"] + 4 * index)[0])

    def imports(self):
        """{patched word: what it names}: ("named", symbol name), ("indexed", module, index) or ("anon", module, raw
        segment tag), as Azahar's loader resolves them (cro_helper.cpp: ResetImport*/ApplyImport*)"""
        h, out = self.h, {}
        for i in range(h["ImportNamedSymbolNum"]):
            name_off, batch = struct.unpack_from("<II", self.data, h["ImportNamedSymbolTableOffset"] + 8 * i)
            for t in self.batch(batch):
                out[t] = ("named", self.cstr(name_off))
        for m in range(h["ImportModuleNum"]):
            name_off, idx_off, idx_num, anon_off, anon_num = struct.unpack_from(
                "<5I", self.data, h["ImportModuleTableOffset"] + 20 * m)
            module = self.cstr(name_off)
            for i in range(idx_num):
                index, batch = struct.unpack_from("<II", self.data, idx_off + 8 * i)
                for t in self.batch(batch):
                    out[t] = ("indexed", module, index)
            for i in range(anon_num):
                tag, batch = struct.unpack_from("<II", self.data, anon_off + 8 * i)
                for t in self.batch(batch):
                    out[t] = ("anon", module, tag)
        return out

    def exports(self):
        out = set()
        for i in range(self.h["ExportNamedSymbolNum"]):
            out.add(self.tag(struct.unpack_from("<I", self.data, self.h["ExportNamedSymbolTableOffset"] + 8 * i + 4)[0]))
        for i in range(self.h["ExportIndexedSymbolNum"]):
            out.add(self.tag(struct.unpack_from("<I", self.data, self.h["ExportIndexedSymbolTableOffset"] + 4 * i)[0]))
        for f in ("OnLoadSegmentTag", "OnExitSegmentTag", "OnUnresolvedSegmentTag", "UnkSegmentTag"):
            if self.h[f] != 0xFFFFFFFF:
                out.add(self.tag(self.h[f]))
        # an odd export is a Thumb function (address | 1): the function just below
        out = {a for a in out if a is not None and self.code_start <= a < self.code_end}
        return {a & ~1 for a in out}, {a & ~1 for a in out if a & 1}

    def code_pointers(self):
        """the places in the code segment its internal relocations write (vtables, callbacks: the loader writes the
        address there), and the Thumb ones among them (address | 1)"""
        out, thumb = set(), set()
        for i in range(self.h["InternalRelocationNum"]):
            _tag, _kind, seg, _p1, _p2, addend = struct.unpack_from(
                "<IBBBBI", self.data, self.h["InternalRelocationTableOffset"] + 12 * i)
            if seg < len(self.segments) and self.segments[seg][2] == 0:
                t = self.segments[seg][0] + addend
                if self.code_start <= t < self.code_end:
                    out.add(t & ~1)
                    if t & 1:
                        thumb.add(t & ~1)
        return out, thumb

    def functions(self):
        """({start: kind}, the Thumb starts): the module's functions, by file offset"""
        exported, thumb = self.exports()
        kinds = {a: "export" for a in exported}
        pointed, pointed_thumb = self.code_pointers()
        word = lambda a: struct.unpack_from("<I", self.data, a)[0]
        opens = lambda a: (a in pointed_thumb and self.data[a + 1] == 0xB5 or a not in pointed_thumb and (
            word(a) >> 16 == 0xE92D or word(a) == 0xE1A00000 and word(a + 4) >> 16 == 0xE92D))
        # a relocated code address is a function when it opens on a push (or nop, push); the others may be labels
        # inside a function (jump tables, exception landing pads: DllBackGround's 001344, an epilogue): they are
        # functions only where no function walked from the sure starts covers them (see below)
        for a in pointed:
            if opens(a):
                kinds.setdefault(a, "pointer")
                if a in pointed_thumb:
                    thumb.add(a)
        for a in range(self.code_start, self.code_end - 3, 4):
            w = struct.unpack_from("<I", self.data, a)[0]
            blx = w >> 25 == 0x7D
            if ((w & 0x0F000000) == 0x0B000000 and w >> 28 != 0xF or blx) and a not in self.relocated:
                imm = w & 0xFFFFFF
                t = a + 8 + ((imm - (1 << 24)) if imm & 0x800000 else imm) * 4 + (((w >> 24) & 1) * 2 if blx else 0)
                if self.code_start <= t < self.code_end:
                    kinds[t] = "call"
                    if blx:
                        thumb.add(t)
        if self.code_end > self.code_start:
            kinds.setdefault(self.code_start, "flow")  # the segment opens on a function
            cover = set()
            self.ends = {}
            starts, thumb_found = flow_pass(self.data, 0, self.code_end, set(kinds), thumb, cover, self.ends)
            rest = {a for a in pointed if a not in kinds and a not in cover}
            if rest:  # relocated addresses no walked code covers: functions reached only through a pointer
                for a in rest:
                    kinds[a] = "pointer"
                    if a in pointed_thumb:
                        thumb.add(a)
                self.ends = {}
                starts, thumb_found = flow_pass(self.data, 0, self.code_end, set(kinds), thumb, None, self.ends)
            thumb = thumb_found
            for a in starts:
                kinds.setdefault(a, "flow")
        return kinds, thumb


def load(cro_dir):
    mods = []
    for f in sorted(os.listdir(cro_dir)):
        if f.endswith(".cro"):
            mods.append(Module(os.path.join(cro_dir, f)))
    return mods


def main():
    p = argparse.ArgumentParser()
    p.add_argument("cro_dir")
    p.add_argument("out")
    a = p.parse_args()
    total = 0
    with open(a.out, "w") as f:
        f.write("# module\toffset\tfound by\tisa\tsize\n")
        for m in load(a.cro_dir):
            fn, thumb = m.functions()
            total += len(fn)
            ordered = sorted(fn)
            for i, s in enumerate(ordered):
                nxt = ordered[i + 1] if i + 1 < len(ordered) else m.code_end
                size = min(getattr(m, "ends", {}).get(s, nxt), nxt) - s
                f.write("%s\t%06X\t%s\t%s\t%d\n" % (m.name, s, fn[s], "thumb" if s in thumb else "arm", size))
    print("%d functions in the modules' code segments" % total)


if __name__ == "__main__":
    main()
