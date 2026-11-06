#!/usr/bin/env python3
"""Our rendered frames against Azahar's screenshots (docs/RECOMPILATION_PLAN.md §11.6: the renderer proven by comparison).

  frame_compare.py <our frames dir> <azahar shots>... [--address 1808C800] [--out sheet.png] [--diff-out dir]

Ours: src/platform/VulkanRenderer.c's $HOS_RENDER_DIR frames (frame_<n>_<address>.ppm, the PICA's framebuffer as
it lies, 256 x 512 for the top screen); only those of --address (the top screen's buffer). Azahar's: Pomegrade's
headless shots (400 x 480: the top screen above the bottom one). Our frame is turned into the screen's view (the
framebuffer holds the screen rotated: transposed, the screen's 240 rows from its 256, its 400 columns from 512),
then each shot's closest frame of ours found (on small images, then among its neighbours at full size: the timelines
differ, ours runs at its own pace unless $HOS_TIME_SCALE keeps it at the game's), and the two compared:
the mean absolute difference per channel (0-255) and the share of pixels differing by more than 8.

The images are the owner's game's: they stay in the session's scratch space, never in the repository.
"""
import argparse
import glob
import os

from PIL import Image, ImageChops, ImageStat


def screen_view(path):
    """our framebuffer as the screen shows it: transposed, the 400 x 240 window"""
    im = Image.open(path).convert("RGB").transpose(Image.Transpose.ROTATE_90)
    w, h = im.size  # 512 x 256 after the turn
    return im.crop((w - 400, h - 240, w, h))


def difference(a, b):
    d = ImageChops.difference(a, b)
    mean = sum(ImageStat.Stat(d).mean) / 3
    big = sum(1 for px in d.convert("L").get_flattened_data() if px > 8) / (a.width * a.height)
    return mean, big, d


def main():
    p = argparse.ArgumentParser()
    p.add_argument("ours")
    p.add_argument("shots", nargs="+")
    p.add_argument("--address", default="1808C800")
    p.add_argument("--out")
    p.add_argument("--diff-out")
    p.add_argument("--step", type=int, default=4, help="every n-th frame of ours searched (speed)")
    a = p.parse_args()
    ours = sorted(glob.glob(os.path.join(a.ours, "frame_*_%s.ppm" % a.address)))
    small = [(f, screen_view(f).resize((100, 60))) for f in ours[::a.step]]
    rows = []
    for shot in a.shots:
        ref = Image.open(shot).convert("RGB").crop((0, 0, 400, 240))
        ref_small = ref.resize((100, 60))
        best = min(small, key=lambda fs: difference(ref_small, fs[1])[0])
        # then the frames around it at full size: the closest one (a frame apart moves what sways)
        at = ours.index(best[0])
        around = ours[max(0, at - a.step - 2):at + a.step + 3]
        scored = [(f, screen_view(f)) for f in around]
        best_full = min(scored, key=lambda fm: difference(ref, fm[1])[0])
        best = (best_full[0], None)
        mine = best_full[1]
        mean, big, d = difference(ref, mine)
        rows.append((shot, best[0], mean, big, ref, mine, d))
        print("%s ~ %s: mean difference %.1f / 255, pixels off by more than 8: %.1f %%"
              % (os.path.basename(shot), os.path.basename(best[0]), mean, 100 * big))
    if a.out:
        sheet = Image.new("RGB", (1200, 240 * len(rows)))
        for k, (_, _, _, _, ref, mine, d) in enumerate(rows):
            sheet.paste(ref, (0, 240 * k))
            sheet.paste(mine, (400, 240 * k))
            sheet.paste(d.point(lambda v: min(255, v * 4)), (800, 240 * k))
        sheet.save(a.out)


if __name__ == "__main__":
    main()
