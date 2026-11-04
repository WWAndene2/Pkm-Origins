#!/usr/bin/env python3
"""Step 1 of the plan (docs/RECOMPILATION_PLAN.md §3): reads the player's decrypted cartridge image (.3ds, NCSD) and writes,
OUTSIDE the repository, what the recompilation translates: the ExeFS code decompressed (code.bin, loaded at 0x100000), the
exheader, and the RomFS's CRO modules (the game's other code). Never point OUT inside the repository (CLAUDE.md §1).

  n3ds_extract.py <game.3ds> <out dir> [--list]     --list: print the RomFS's .cro files and the code's size only

Formats (3dbrew's NCSD, NCCH, ExeFS, IVFC/RomFS pages; checked 4 November on Omega Ruby 1.4: code.bin byte-identical to the
one Pomegrade's remake_tool extracts, 5,439,488 bytes, 147 CRO/CRS/CRR; X: 5,156,864 bytes, 135):
  NCSD: partition table at 0x120 (offset, size in 0x200-byte units); partition 0 is the game's NCCH.
  NCCH: ExeFS at 0x1A0, RomFS at 0x1B0 (offset, size in units); exheader at 0x200, its byte 0xD bit 0: code compressed.
  ExeFS: 10 entries (name[8], offset, size) then the files from 0x200; ".code" is reverse-LZ compressed when flagged.
  RomFS: IVFC header; level 3 at the master hash's end aligned to level 3's block; directory and file metadata tables.
"""
import argparse
import os
import struct
import sys

UNIT = 0x200


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def blz_decompress(data):
    """The code's reverse LZ (3dbrew "Code compression"): decoded from the end backwards, the output grown by the footer."""
    footer_top = u32(data, len(data) - 8)
    top, bottom = footer_top & 0xFFFFFF, footer_top >> 24
    out = bytearray(data) + bytearray(u32(data, len(data) - 4))
    src, dst, stop = len(data) - bottom, len(out), len(data) - top
    while src > stop:
        src -= 1
        flags = out[src]
        for _ in range(8):
            if src <= stop:
                break
            if flags & 0x80:
                src -= 2
                value = out[src] | (out[src + 1] << 8)
                size, disp = ((value >> 12) & 0xF) + 3, (value & 0xFFF) + 3
                for _ in range(size):
                    dst -= 1
                    out[dst] = out[dst + disp]
            else:
                src -= 1
                dst -= 1
                out[dst] = out[src]
            flags = (flags << 1) & 0xFF
    return bytes(out)


def read(f, offset, size):
    f.seek(offset)
    return f.read(size)


def romfs_files(f, base):
    """(path, absolute offset, size) of every RomFS file."""
    ivfc = read(f, base, 0x60)
    if ivfc[:4] != b"IVFC":
        raise SystemExit("no IVFC header at the RomFS")
    block = 1 << u32(ivfc, 0x4C)
    lvl3 = base + ((0x60 + u32(ivfc, 0x08) + block - 1) // block) * block
    h = read(f, lvl3, 0x28)
    dmeta = read(f, lvl3 + u32(h, 0x0C), u32(h, 0x10))
    fmeta = read(f, lvl3 + u32(h, 0x1C), u32(h, 0x20))
    data = lvl3 + u32(h, 0x24)
    name = lambda b, o, n: b[o:o + n].decode("utf-16-le")
    out = []

    def walk(d, path):
        child, file = u32(dmeta, d + 8), u32(dmeta, d + 12)
        while file != 0xFFFFFFFF:
            off, size = struct.unpack_from("<QQ", fmeta, file + 8)
            out.append((path + name(fmeta, file + 0x20, u32(fmeta, file + 0x1C)), data + off, size))
            file = u32(fmeta, file + 4)
        while child != 0xFFFFFFFF:
            walk(child, path + name(dmeta, child + 0x18, u32(dmeta, child + 0x14)) + "/")
            child = u32(dmeta, child + 4)

    walk(0, "")
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("game")
    p.add_argument("out")
    p.add_argument("--list", action="store_true")
    a = p.parse_args()
    repo = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
    if os.path.realpath(a.out).startswith(repo + os.sep) or os.path.realpath(a.out) == repo:
        raise SystemExit("OUT is inside the repository: the game's files never go there (CLAUDE.md §1)")
    with open(a.game, "rb") as f:
        ncsd = read(f, 0, 0x200)
        if ncsd[0x100:0x104] != b"NCSD":
            raise SystemExit("not an NCSD (.3ds) image")
        part = u32(ncsd, 0x120) * UNIT
        ncch = read(f, part, 0x200)
        if ncch[0x100:0x104] != b"NCCH":
            raise SystemExit("partition 0 is not an NCCH (is the image decrypted?)")
        exheader = read(f, part + 0x200, 0x800)
        exefs = part + u32(ncch, 0x1A0) * UNIT
        header = read(f, exefs, 0x200)
        code = None
        for k in range(10):
            n = header[k * 16:k * 16 + 8].rstrip(b"\0")
            if n == b".code":
                code = read(f, exefs + 0x200 + u32(header, k * 16 + 8), u32(header, k * 16 + 12))
        if code is None:
            raise SystemExit("no .code in the ExeFS")
        if exheader[0xD] & 1:
            code = blz_decompress(code)
        files = romfs_files(f, part + u32(ncch, 0x1B0) * UNIT)
        cros = [x for x in files if x[0].lower().endswith((".cro", ".crs", ".crr"))]
        print("title %016X: code %d bytes, %d RomFS files, %d CRO/CRS/CRR"
              % (struct.unpack_from("<Q", ncch, 0x118)[0], len(code), len(files), len(cros)))
        if a.list:
            for path, _, size in cros:
                print("  %8d  %s" % (size, path))
            return
        os.makedirs(os.path.join(a.out, "cro"), exist_ok=True)
        open(os.path.join(a.out, "code.bin"), "wb").write(code)
        open(os.path.join(a.out, "exheader.bin"), "wb").write(exheader)
        for path, off, size in cros:
            open(os.path.join(a.out, "cro", os.path.basename(path)), "wb").write(read(f, off, size))
        print("written to", a.out)


if __name__ == "__main__":
    sys.exit(main())
