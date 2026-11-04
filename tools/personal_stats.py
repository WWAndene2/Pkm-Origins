#!/usr/bin/env python3
"""Balance work, step 1 (docs/FEATURE_IDEAS.md, "Équilibrage", the method): the base stats of every species, read from
the player's copy of the game, measured instead of remembered. Prints to the terminal or writes a TSV OUTSIDE the
repository (the stats are the game's data, CLAUDE.md §1); only the analysis is committed.

  personal_stats.py <game.3ds> [--out stats.tsv] [--game oras|xy]

The personal archive: RomFS a/1/9/5 in Omega Ruby, a/2/1/8 in X (GARC; one 0x50-byte (ORAS) or 0x40-byte (XY) entry per
species and form, then one file holding them all, skipped). Entry: HP, Atk, Def, Spe, SpA, SpD at 0-5; types at 6-7;
abilities at 0x18-0x1A; the form count at 0x20 and the first form's entry at 0x1C (u16). Checked 4 November: entry 1 reads
45/49/49/45/65/65 Grass/Poison (Bulbizarre).
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import n3ds_extract as n3ds  # noqa: E402

TYPES = ["Normal", "Combat", "Vol", "Poison", "Sol", "Roche", "Insecte", "Spectre", "Acier", "Feu", "Eau", "Plante",
         "Électrik", "Psy", "Glace", "Dragon", "Ténèbres", "Fée"]
ARCHIVE = {"oras": ("a/1/9/5", 0x50), "xy": ("a/2/1/8", 0x40)}


def garc_files(b):
    """The files of a GARC (header, FATO, FATB, FIMB), first sub-file of each entry."""
    if b[:4] != b"CRAG":
        raise SystemExit("not a GARC")
    header = struct.unpack_from("<I", b, 4)[0]
    data = struct.unpack_from("<I", b, 0x10)[0]
    fato = header
    count = struct.unpack_from("<H", b, fato + 8)[0]
    fatb = fato + struct.unpack_from("<I", b, fato + 4)[0]
    out = []
    for k in range(count):
        at = fatb + 12 + struct.unpack_from("<I", b, fato + 12 + 4 * k)[0]
        start, end = struct.unpack_from("<II", b, at + 4)
        out.append(b[data + start:data + end])
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("game")
    p.add_argument("--out")
    p.add_argument("--game", dest="which", choices=ARCHIVE, default="oras")
    a = p.parse_args()
    repo = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
    if a.out and os.path.realpath(a.out).startswith(repo + os.sep):
        raise SystemExit("--out is inside the repository: the game's data never goes there (CLAUDE.md §1)")
    path, size = ARCHIVE[a.which]
    with open(a.game, "rb") as f:
        ncsd = n3ds.read(f, 0, 0x200)
        part = n3ds.u32(ncsd, 0x120) * n3ds.UNIT
        ncch = n3ds.read(f, part, 0x200)
        files = {p_: (o, s) for p_, o, s in n3ds.romfs_files(f, part + n3ds.u32(ncch, 0x1B0) * n3ds.UNIT)}
        off, length = files[path]
        entries = [e for e in garc_files(n3ds.read(f, off, length)) if len(e) == size]
    rows = []
    for idx, e in enumerate(entries):
        hp, atk, de, spe, spa, spd, t1, t2 = e[:8]
        rows.append((idx, hp, atk, de, spa, spd, spe, hp + atk + de + spa + spd + spe, TYPES[t1],
                     TYPES[t2] if t2 != t1 else "", e[0x18], e[0x19], e[0x1A]))
    lines = ["index\tPV\tAtq\tDéf\tAtqSpé\tDéfSpé\tVit\tTotal\tType1\tType2\tTalent1\tTalent2\tTalentCaché"]
    lines += ["\t".join(str(x) for x in r) for r in rows]
    if a.out:
        open(a.out, "w").write("\n".join(lines) + "\n")
        print("%d entries written to %s" % (len(rows), a.out))
    else:
        print("\n".join(lines))


if __name__ == "__main__":
    main()
