#!/usr/bin/env python3
"""The function list of the CRO modules (step 4 of docs/RECOMPILATION_PLAN.md §7), as function_list.py does for the .code:
every module's code segment split into functions, so the shared count covers the modules too.

  cro_functions.py <cro dir> <out.tsv>

A CRO's layout is Azahar's loader's (azahar/src/core/hle/service/ldr_ro/cro_helper.h), read the way Pomegrade's
tools/remake/prototype/cro_dis.py reads it: a 0x80-byte hash, the header's u32 fields, a segment table (offset, size,
type; offsets are file offsets in the file as stored). Starts: the targets of bl / blx inside the code segment, the
symbols the module exports into it, then function_list.py's flow pass over the segment.

out.tsv (names and offsets only, no game data): module, offset in the file, how it was found (call / export / flow).
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
        self.name = os.path.basename(path)[:-4]
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

    def exports(self):
        out = set()
        for i in range(self.h["ExportNamedSymbolNum"]):
            out.add(self.tag(struct.unpack_from("<I", self.data, self.h["ExportNamedSymbolTableOffset"] + 8 * i + 4)[0]))
        for i in range(self.h["ExportIndexedSymbolNum"]):
            out.add(self.tag(struct.unpack_from("<I", self.data, self.h["ExportIndexedSymbolTableOffset"] + 4 * i)[0]))
        for f in ("OnLoadSegmentTag", "OnExitSegmentTag", "OnUnresolvedSegmentTag", "UnkSegmentTag"):
            if self.h[f] != 0xFFFFFFFF:
                out.add(self.tag(self.h[f]))
        return {a for a in out if a is not None and self.code_start <= a < self.code_end and a % 4 == 0}

    def functions(self):
        """{start: kind}: the module's functions, by file offset"""
        kinds = {a: "export" for a in self.exports()}
        for a in range(self.code_start, self.code_end - 3, 4):
            w = struct.unpack_from("<I", self.data, a)[0]
            if (w & 0x0F000000) == 0x0B000000 and w >> 28 != 0xF and a not in self.relocated:
                imm = w & 0xFFFFFF
                t = a + 8 + ((imm - (1 << 24)) if imm & 0x800000 else imm) * 4
                if self.code_start <= t < self.code_end:
                    kinds[t] = "call"
        if self.code_end > self.code_start:
            kinds.setdefault(self.code_start, "flow")  # the segment opens on a function
            for a in flow_pass(self.data, 0, self.code_end, set(kinds)):
                kinds.setdefault(a, "flow")
        return kinds


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
        f.write("# module\toffset\tfound by\n")
        for m in load(a.cro_dir):
            fn = m.functions()
            total += len(fn)
            for s in sorted(fn):
                f.write("%s\t%06X\t%s\n" % (m.name, s, fn[s]))
    print("%d functions in the modules' code segments" % total)


if __name__ == "__main__":
    main()
