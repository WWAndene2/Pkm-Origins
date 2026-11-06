#!/usr/bin/env python3
"""A contact sheet of our rendered frames, to look at a scene by eye (docs/RECOMPILATION_PLAN.md §11.6).

  frame_sheet.py <frames dir> --out sheet.png [--from N] [--to N] [--every 20] [--columns 4] [--scale 1.0]
                 [--address 1808C800] [--crop x,y,w,h --zoom 3]

The frames: src/platform/VulkanRenderer.c's $HOS_RENDER_DIR (frame_<n>_<address>.ppm), shown as the screen does
(tools/frame_compare.py's screen_view), each labelled with its frame number. --from / --to bound the frame numbers of
the file names; --every keeps one frame in n of those. --crop keeps a region of each frame (screen coordinates, 400 x
240) and --zoom enlarges it with no smoothing: a face or a line of text looked at closely, across frames.

Why: on 6 November a whole-screen score called the opening close to Azahar while its faces, eyes and title text were
wrong; a sheet of the frames, looked at, showed them at once. Look at the frames, not only at a score.

The images are the owner's game's: they stay in the session's scratch space, never in the repository.
"""
import argparse
import glob
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from frame_compare import label, screen_view  # noqa: E402


def number(path):
    return int(os.path.basename(path).split("_")[1])


def main():
    p = argparse.ArgumentParser()
    p.add_argument("frames")
    p.add_argument("--out", required=True)
    p.add_argument("--address", default="1808C800")
    p.add_argument("--from", dest="first", type=int, default=0)
    p.add_argument("--to", dest="last", type=int, default=1 << 30)
    p.add_argument("--every", type=int, default=20)
    p.add_argument("--columns", type=int, default=4)
    p.add_argument("--scale", type=float, default=1.0, help="each cell's size (1: 400 x 240)")
    p.add_argument("--crop", help="x,y,w,h of the screen kept")
    p.add_argument("--zoom", type=int, default=1, help="with --crop: enlarged this many times (nearest)")
    a = p.parse_args()
    frames = [f for f in sorted(glob.glob(os.path.join(a.frames, "frame_*_%s.ppm" % a.address)))
              if a.first <= number(f) <= a.last][::a.every]
    if not frames:
        raise SystemExit("no frames of %s in %s between %d and %d" % (a.address, a.frames, a.first, a.last))
    box = tuple(int(v) for v in a.crop.split(",")) if a.crop else (0, 0, 400, 240)
    cw, ch = int(box[2] * a.zoom * a.scale), int(box[3] * a.zoom * a.scale)
    rows = (len(frames) + a.columns - 1) // a.columns
    sheet = Image.new("RGB", (a.columns * (cw + 2), rows * (ch + 2)), (40, 40, 40))
    for k, f in enumerate(frames):
        im = screen_view(f).crop((box[0], box[1], box[0] + box[2], box[1] + box[3]))
        im = im.resize((cw, ch), Image.Resampling.NEAREST if a.zoom > 1 else Image.Resampling.BILINEAR)
        x, y = (k % a.columns) * (cw + 2), (k // a.columns) * (ch + 2)
        sheet.paste(im, (x, y))
        label(sheet, x, y, "frame %d" % number(f))
    sheet.save(a.out)
    print("%d frames (%d to %d) in %s" % (len(frames), number(frames[0]), number(frames[-1]), a.out))


if __name__ == "__main__":
    main()
