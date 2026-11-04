#!/usr/bin/env python3
"""The system font file the runtime's APT GetSharedFont gives the game (src/hos/service/Apt.c): <out> in the format
Azahar's legacy loader reads (a 0x80-byte header: status 2, region, decompressed size; then the decompressed BCFNT,
its magic CFNU), made from a font archive's RomFS (level 3) as Azahar's LoadSharedFont does: its cbf_std.bcfnt.lz,
LZ11-decompressed (service/apt/apt.cpp).

  make_shared_font.py <archive> <out shared_font.bin>

The archive: the player's own console's system font archive (title 0004009B00014002, its RomFS), or the open-source
replacement Azahar ships (externals/open_source_archives/include/shared_font.app.romfs.h, a C array: read as such).
The output is not committed (it is the console's or Azahar's data).
"""
import re
import struct
import sys


def romfs_file(romfs, name):
    """a file's bytes from a RomFS level-3 image, by name (the file metadata table, scanned)"""
    meta_off, meta_size, data_off = struct.unpack_from("<I", romfs, 0x1C)[0], struct.unpack_from("<I", romfs, 0x20)[0], \
        struct.unpack_from("<I", romfs, 0x24)[0]
    at = meta_off
    while at < meta_off + meta_size:
        _parent, _sibling, off, size, _hash, name_len = struct.unpack_from("<IIQQII", romfs, at)
        entry_name = romfs[at + 0x20:at + 0x20 + name_len].decode("utf-16-le")
        if entry_name == name:
            return romfs[data_off + off:data_off + off + size]
        at += 0x20 + ((name_len + 3) & ~3)
    raise SystemExit("no %s in the archive" % name)


def lz11(data):
    size = struct.unpack_from("<I", data, 0)[0]
    assert size & 0xFF == 0x11, "not LZ11"
    size >>= 8
    out, i = bytearray(), 4
    while len(out) < size:
        flags = data[i]
        i += 1
        for bit in range(7, -1, -1):
            if len(out) >= size:
                break
            if not flags >> bit & 1:
                out.append(data[i])
                i += 1
                continue
            b1 = data[i]
            kind = b1 >> 4
            if kind == 0:
                b2, b3 = data[i + 1], data[i + 2]
                length, offset, i = (((b1 & 0xF) << 4) | (b2 >> 4)) + 0x11, (((b2 & 0xF) << 8) | b3) + 1, i + 3
            elif kind == 1:
                b2, b3, b4 = data[i + 1], data[i + 2], data[i + 3]
                length = (((b1 & 0xF) << 12) | (b2 << 4) | (b3 >> 4)) + 0x111
                offset, i = (((b3 & 0xF) << 8) | b4) + 1, i + 4
            else:
                b2 = data[i + 1]
                length, offset, i = kind + 1, (((b1 & 0xF) << 8) | b2) + 1, i + 2
            for _ in range(length):
                out.append(out[-offset])
    return bytes(out)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    raw = open(src, "rb").read()
    if src.endswith(".h"):  # the C array Azahar ships
        body = raw.decode("ascii", "replace")
        raw = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", body[body.index("{"):]))
    font = lz11(romfs_file(raw, "cbf_std.bcfnt.lz"))
    header = struct.pack("<III", 2, 1, len(font)) + bytes(0x80 - 12)  # status: loaded; region 1 (JPN / EUR / USA)
    font = font[:3] + b"U" + font[4:]  # (the magic CFNT becomes CFNU, as Azahar writes it)
    open(dst, "wb").write(header + font)
    print("%s: %d bytes (font %d)" % (dst, 0x80 + len(font), len(font)))


if __name__ == "__main__":
    main()
