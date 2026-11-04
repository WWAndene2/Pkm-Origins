#!/usr/bin/env python3
"""A system data archive the runtime's fs:USER serves (archive 0x2345678A, src/hos/service/Fs.c): <out>, the archive's
RomFS image as is, which the game reads and parses itself. Made from one of the open-source replacements Azahar ships
and serves when the console's own is missing (externals/open_source_archives/include/<name>.app.romfs.h, a C array:
service file_sys/archive_ncch.cpp), or copied from the player's console. Not committed (Azahar's or the console's data).

  make_system_archive.py <archive .h or .romfs> <extracted dir> <title id>

The file is written to <extracted dir>/<title id>.romfs, the name Fs.c looks for. Title ids (3dbrew's title list):
000400DB00010302 the bad word list, 0004009B00010202 the Mii data, 0004009B00010402 the country list.
"""
import re
import sys


def main():
    src, out_dir, title = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
    raw = open(src, "rb").read()
    if src.endswith(".h"):  # the C array Azahar ships
        body = raw.decode("ascii", "replace")
        raw = bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", body[body.index("{"):]))
    dst = "%s/%016X.romfs" % (out_dir, title)
    open(dst, "wb").write(raw)
    print("%s: %d bytes" % (dst, len(raw)))


if __name__ == "__main__":
    main()
