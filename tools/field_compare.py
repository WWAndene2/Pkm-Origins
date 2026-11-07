#!/usr/bin/env python3
"""Our last top-screen frame against Azahar's shot (docs/AGENT_HANDOFF.md, step 4 of 7 November): the frame's 400 x 240
screen cut out of its 256 x 512 render target and turned upright, Azahar's top screen cut out of its 400 x 480 shot,
their mean colours printed and the two side by side in a sheet (look at it: a mean hides local faults).

  field_compare.py <our frames dir> <azahar shot .png> --out sheet.png

Azahar's shot: Pomegrade's run_local.sh with POMEGRADE_SAVE set to the same save and the script
"draw every 15;field 300;wait 120;draw on;wait 8;shot f1", started at the same time as tools/field_run.sh (the field's
light follows the real clock). A black shot means Azahar's field was not up yet: take it again."""
import argparse
import os
import re

from PIL import Image, ImageStat


def main():
    p = argparse.ArgumentParser()
    p.add_argument("frames")
    p.add_argument("shot")
    p.add_argument("--out", required=True)
    a = p.parse_args()
    frames = sorted((int(re.match(r"frame_(\d+)_", f).group(1)), f)
                    for f in os.listdir(a.frames) if f.endswith("_1808C800.ppm"))
    ours = Image.open(os.path.join(a.frames, frames[-1][1])).convert("RGB").crop((0, 112, 240, 512)).transpose(Image.ROTATE_90)
    theirs = Image.open(a.shot).convert("RGB").crop((0, 0, 400, 240))
    for name, im in (("azahar", theirs), ("ours", ours)):
        print(name, [round(x, 1) for x in ImageStat.Stat(im).mean])
    sheet = Image.new("RGB", (800, 240))
    sheet.paste(theirs, (0, 0))
    sheet.paste(ours, (400, 0))
    sheet.resize((1600, 480)).save(a.out)


if __name__ == "__main__":
    main()
