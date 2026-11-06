#!/usr/bin/env python3
"""Items put in an Omega Ruby / Alpha Sapphire save's bag, for test saves (the matching needs RAM dumps from moments the
owner's saves do not reach yet: a capture, a shop; they hold no Poke Balls). Moved from Pomegrade's
tools/remake/prototype/oras_save.py (its "item" command, commit 31f59fe0, made there by mistake on 5 November: this is
Pkm Origins' tool).

  oras_save_items.py <save> <out> <pouch> <item> <count>

pouch: items, key, tm, medicine, berry. The bag is block 1 of the save (MYITEM), its pouches at +0, +0x640, +0x7C0,
+0x970, +0xA70, each a list of (u16 item, u16 count) entries (PKHeX's ORAS layout; checked on the owner's save: its
Potion, item 17, first in medicine). The item is added to its own entry, or the first empty one, then every block's
checksum (CRC-16 CCITT, in the block table of the footer) is written again. The save is the owner's: it stays in the
session's scratch space (CLAUDE.md §1).
"""
import struct
import sys

POUCHES = {"items": (0x000, 400), "key": (0x640, 96), "tm": (0x7C0, 108), "medicine": (0x970, 64), "berry": (0xA70, 72)}


def table_at(data):
    """the block table: the footer's + 0x14, the footer the file's last 0x200 bytes"""
    return len(data) - 0x200 + 0x14


def crc16(data):
    c = 0xFFFF
    for x in data:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def blocks(data):
    """[(index, id, offset, size)] from the table (each block 0x200-aligned)"""
    out, offset = [], 0
    for i, at in enumerate(range(table_at(data), len(data) - 7, 8)):
        size, bid, _ = struct.unpack_from("<IHH", data, at)
        if size == 0 or offset + size > table_at(data):
            break
        out.append((i, bid, offset, size))
        offset = (offset + size + 0x1FF) & ~0x1FF
    return out


def read(path):
    data = bytearray(open(path, "rb").read())
    if len(data) < 0x76000 or len(data) % 0x200 or data[table_at(data) - 4:table_at(data)] != b"FEEB":
        sys.exit(f"{path}: not an ORAS save (0x76000 bytes or a grown one, FEEB before the table)")
    return data


def write_checksums(data):
    for i, _, offset, size in blocks(data):
        struct.pack_into("<H", data, table_at(data) + i * 8 + 6, crc16(data[offset:offset + size]))


def main():
    if len(sys.argv) != 6 or sys.argv[3] not in POUCHES:
        sys.exit(__doc__)
    path, out, pouch, item, count = sys.argv[1:]
    data = read(path)
    bag = next((b for b in blocks(data) if b[0] == 1), None)
    if bag is None:
        sys.exit("no block 1 (the bag)")
    start, n = POUCHES[pouch]
    item, count = int(item, 0), int(count, 0)
    for k in range(n):
        at = bag[2] + start + 4 * k
        i, c = struct.unpack_from("<HH", data, at)
        if i == item or i == 0:
            struct.pack_into("<HH", data, at, item, min(999, (c if i == item else 0) + count))
            break
    else:
        sys.exit(f"pouch {pouch} full")
    write_checksums(data)
    open(out, "wb").write(data)
    print(f"{out}: item {item} x{count} in {pouch} (entry {k}), checksums rewritten")


if __name__ == "__main__":
    main()
