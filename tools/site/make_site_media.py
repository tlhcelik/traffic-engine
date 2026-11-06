"""Makes the images of the web site (docs/assets/img) from the output of
capture_media.ps1: still images in WebP format and GIF animations.

    python tools/site/make_site_media.py <media folder> docs/assets/img

Needs Python 3 with Pillow and numpy.
"""
import glob
import os
import sys

import numpy as np
from PIL import Image


def view_offset(win, view):
    """Position of the view image in the window image. The window image does
    not have the overlays of the view (clock, legend, map attribution)."""
    w = np.asarray(win.convert('L'), dtype=np.float32)
    v = np.asarray(view.convert('L'), dtype=np.float32)
    ph, pw = 160, 240
    vy, vx = v.shape[0] // 2 - ph // 2, v.shape[1] // 2 - pw // 2
    patch = v[vy:vy + ph, vx:vx + pw]
    best = None
    for oy in range(60, 130):
        for ox in range(30, 90):
            region = w[oy + vy:oy + vy + ph, ox + vx:ox + vx + pw]
            if region.shape != patch.shape:
                continue
            d = float(np.mean(np.abs(region - patch)))
            if best is None or d < best[0]:
                best = (d, ox, oy)
    return best[1], best[2]


def composite(folder, name):
    """The window image with the view image (and its overlays) in it."""
    win = Image.open(os.path.join(folder, name + '.png')).convert('RGB')
    view = Image.open(os.path.join(folder, name + '_view.png')).convert('RGB')
    win.paste(view, view_offset(win, view))
    return win


def save_webp(im, out, name, width=None):
    if width and im.width > width:
        im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
    path = os.path.join(out, name + '.webp')
    im.save(path, 'WEBP', quality=82, method=6)
    print(path, im.size, os.path.getsize(path) // 1024, 'KB')


def make_gif(folder, prefix, path, crop, size, fps, colors=200):
    """GIF from the frames <prefix>_f000.png, ... with one palette for all
    frames (no flicker, a smaller file)."""
    files = sorted(glob.glob(os.path.join(folder, prefix + '_f*.png')))
    frames = [Image.open(f).convert('RGB').crop(crop).resize(size, Image.LANCZOS) for f in files]
    sample = frames[:: max(1, len(frames) // 6)]
    mosaic = Image.new('RGB', (size[0], size[1] * len(sample)))
    for i, im in enumerate(sample):
        mosaic.paste(im, (0, i * size[1]))
    palette = mosaic.quantize(colors=colors, method=Image.MEDIANCUT)
    out = [im.quantize(palette=palette, dither=Image.Dither.NONE) for im in frames]
    out[0].save(path, save_all=True, append_images=out[1:], duration=int(1000 / fps), loop=0, optimize=True, disposal=1)
    print(path, len(out), 'frames', os.path.getsize(path) // 1024, 'KB')


def main():
    media, out = sys.argv[1], sys.argv[2]
    shots = os.path.join(media, 'shots')
    os.makedirs(out, exist_ok=True)

    save_webp(composite(shots, 'hero'), out, 'hero', 1600)
    save_webp(composite(os.path.join(media, 'cong'), 'c'), out, 'congestion-panel', 1600)
    for lang in ('en', 'tr'):
        # The junction, the information box of the car and the property panel
        save_webp(composite(shots, 'wait_' + lang).crop((560, 225, 1600, 870)), out, 'wait-reason-' + lang)
    save_webp(Image.open(os.path.join(shots, 'osm_dialog.png')).convert('RGB'), out, 'osm-import')
    save_webp(Image.open(os.path.join(shots, 'program_dialog.png')).convert('RGB'), out, 'light-program')
    save_webp(composite(shots, 'charts').crop((53, 428, 1600, 878)), out, 'charts')
    signs = Image.open(os.path.join(shots, 'signs.png')).convert('RGB')
    save_webp(signs.crop((0, 0, signs.width, 690)), out, 'signs', 1200)
    Image.open(os.path.join(shots, 'splash.png')).save(os.path.join(out, 'splash.png'), optimize=True)

    # Animations. The crops remove the clock box and the help line of the view.
    make_gif(os.path.join(media, 'junction'), 'j', os.path.join(out, 'junction.gif'), (116, 42, 1156, 627), (800, 450), 10, 160)
    make_gif(os.path.join(media, 'map'), 'k', os.path.join(out, 'real-roads.gif'), (24, 42, 1273, 744), (1120, 630), 10)
    make_gif(os.path.join(media, 'cong'), 'c', os.path.join(out, 'congestion.gif'), (0, 0, 1273, 458), (1120, 403), 8)


if __name__ == '__main__':
    main()
