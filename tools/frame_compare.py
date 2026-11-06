#!/usr/bin/env python3
"""Our rendered frames against Azahar's screenshots (docs/RECOMPILATION_PLAN.md §11.6: the renderer proven by comparison).

  frame_compare.py <our frames dir> <azahar shots>... [--address 1808C800] [--out sheet.png] [--zoom-out dir]
                   [--any-order] [--window 400] [--step 4]

Ours: src/platform/VulkanRenderer.c's $HOS_RENDER_DIR frames (frame_<n>_<address>.ppm, the PICA's framebuffer as
it lies, 256 x 512 for the top screen); only those of --address (the top screen's buffer). Azahar's: Pomegrade's
headless shots (400 x 480: the top screen above the bottom one). Our frame is turned into the screen's view (the
framebuffer holds the screen rotated), then each shot's closest frame of ours found (on small images, then among its
neighbours at full size).

The shots are taken in time order, as given (list them in order: a shell sorts z10 before z2): each is searched at or
after the previous one's frame, within --window frames. A scene whose objects come and go (Latios and Latias across
the island) otherwise matched a moment without them anywhere in the opening. --any-order searches all.

Per shot, two measures:
  mean   the mean absolute difference per channel (0-255) over the screen;
  worst  the same over the worst 32 x 32 tile. A glitch on a small object (a face, the eyes, a line of text) moves the
         mean by little (6 November: the opening's textures upside down scored 12-15 on the pond, the faces and the
         title text wrong); the worst tile shows it. Read both, and look at the sheet: a score is not a review.

--out: one row per shot, labelled: AZAHAR | OURS | DIFF x4 (the worst tile outlined in red in all three).
--zoom-out: per shot, the worst tile and its surroundings (96 x 96) of both, enlarged 4 times, side by side.
Azahar's headless shots have artefacts of their own (black hatching on outlines, 6 November): they are Azahar's, not
ours; the labels say which column is which.

The images are the owner's game's: they stay in the session's scratch space, never in the repository.
"""
import argparse
import glob
import os

from PIL import Image, ImageChops, ImageDraw, ImageStat

TILE = 32


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


def worst_tile(d):
    """the 32 x 32 tile of the difference image (on a half-tile grid) with the highest mean, and that mean"""
    best = (0.0, (0, 0))
    for y in range(0, d.height - TILE + 1, TILE // 2):
        for x in range(0, d.width - TILE + 1, TILE // 2):
            m = sum(ImageStat.Stat(d.crop((x, y, x + TILE, y + TILE))).mean) / 3
            if m > best[0]:
                best = (m, (x, y))
    return best


def label(sheet, x, y, text):
    draw = ImageDraw.Draw(sheet)
    draw.rectangle((x, y, x + 6 * len(text) + 8, y + 14), fill=(0, 0, 0))
    draw.text((x + 4, y + 2), text, fill=(255, 255, 0))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("ours")
    p.add_argument("shots", nargs="+")
    p.add_argument("--address", default="1808C800")
    p.add_argument("--out", help="the comparison sheet (png)")
    p.add_argument("--zoom-out", help="a directory: each shot's worst tile, enlarged")
    p.add_argument("--step", type=int, default=4, help="every n-th frame of ours searched first (speed)")
    p.add_argument("--any-order", action="store_true", help="each shot searched among all our frames")
    p.add_argument("--window", type=int, default=400, help="frames of ours searched after the previous shot's")
    a = p.parse_args()
    ours = sorted(glob.glob(os.path.join(a.ours, "frame_*_%s.ppm" % a.address)))
    if not ours:
        raise SystemExit("no frames of %s in %s" % (a.address, a.ours))
    small = [(k, screen_view(f).resize((100, 60))) for k, f in enumerate(ours) if k % a.step == 0]
    rows = []
    after = 0
    for shot in a.shots:
        ref = Image.open(shot).convert("RGB").crop((0, 0, 400, 240))
        ref_small = ref.resize((100, 60))
        pool = small if a.any_order else [s for s in small if after <= s[0] < after + a.window] or small
        at = min(pool, key=lambda s: difference(ref_small, s[1])[0])[0]
        # then the frames around it at full size: the closest one (a frame apart moves what sways)
        around = range(max(0, at - a.step - 2), min(len(ours), at + a.step + 3))
        k, mine = min(((k, screen_view(ours[k])) for k in around), key=lambda km: difference(ref, km[1])[0])
        after = k
        mean, big, d = difference(ref, mine)
        worst, (tx, ty) = worst_tile(d)
        rows.append((shot, ours[k], mean, worst, (tx, ty), ref, mine, d))
        print("%s ~ %s: mean %.1f, worst tile %.1f at %d,%d (/ 255); pixels off by more than 8: %.1f %%"
              % (os.path.basename(shot), os.path.basename(ours[k]), mean, worst, tx, ty, 100 * big))
    if a.out:
        sheet = Image.new("RGB", (1200, 240 * len(rows)))
        for r, (shot, frame, mean, worst, (tx, ty), ref, mine, d) in enumerate(rows):
            y = 240 * r
            sheet.paste(ref, (0, y))
            sheet.paste(mine, (400, y))
            sheet.paste(d.point(lambda v: min(255, v * 4)), (800, y))
            draw = ImageDraw.Draw(sheet)
            for x in (0, 400, 800):
                draw.rectangle((x + tx, y + ty, x + tx + TILE - 1, y + ty + TILE - 1), outline=(255, 0, 0))
            label(sheet, 0, y, "AZAHAR " + os.path.basename(shot))
            label(sheet, 400, y, "OURS frame " + os.path.basename(frame)[6:10])
            label(sheet, 800, y, "DIFF x4  mean %.1f  worst %.1f" % (mean, worst))
        sheet.save(a.out)
    if a.zoom_out:
        os.makedirs(a.zoom_out, exist_ok=True)
        for shot, frame, mean, worst, (tx, ty), ref, mine, d in rows:
            x0, y0 = max(0, min(400 - 96, tx - 32)), max(0, min(240 - 96, ty - 32))
            box = (x0, y0, x0 + 96, y0 + 96)
            z = Image.new("RGB", (2 * 384 + 8, 384 + 16))
            z.paste(ref.crop(box).resize((384, 384), Image.Resampling.NEAREST), (0, 16))
            z.paste(mine.crop(box).resize((384, 384), Image.Resampling.NEAREST), (392, 16))
            label(z, 0, 0, "AZAHAR")
            label(z, 392, 0, "OURS")
            z.save(os.path.join(a.zoom_out, os.path.splitext(os.path.basename(shot))[0] + "_zoom.png"))


if __name__ == "__main__":
    main()
