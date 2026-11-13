#!/usr/bin/env python3
# make_signs.py - paints the clinic's hand-made signs: original text and an original mark, one 2048 atlas.
# Writes CWC_Signs_d.png, _n.png, _s.png and _g.png (glow) into <out>. Needs numpy and Pillow.
#   python3 -I make_signs.py <out dir>
# Rects are shared with signs_nif.py (PANELS). No game assets are read or copied.
import os, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1]
SIZE = 2048
FONT = '/usr/share/fonts/truetype/liberation/LiberationSansNarrow-Bold.ttf'
PANELS = {                 # name: (x, y, w, h) in atlas pixels
    'cash':   (0, 0, 512, 768),
    'knock':  (512, 0, 512, 768),
    'jars':   (1024, 0, 512, 768),
    'logo':   (1536, 0, 512, 512),
    'still':  (0, 768, 1024, 512),
    'sharps': (1024, 768, 1024, 512),
    'edge':   (0, 1280, 512, 256),
}
rng = np.random.default_rng(20261109)

def noise(w, h, scale, amp):
    n = rng.random((max(2, h // scale), max(2, w // scale)))
    return np.asarray(Image.fromarray((n * 255).astype('uint8')).resize((w, h), Image.BICUBIC), dtype=float) / 255 * amp

def board(w, h, kind):
    if kind == 'plywood':
        grain = noise(w, h, 2, 22) + noise(w, h, 40, 16) + np.sin(np.linspace(0, 60, h))[:, None] * 5
        rgb = np.stack([62 + grain, 46 + grain * .8, 30 + grain * .6], 2)
    else:                  # rusted sheet steel
        rust = noise(w, h, 3, 40) + noise(w, h, 40, 70)
        rgb = np.stack([70 + rust * 1.1, 62 + rust * .6, 58 + rust * .25], 2)
    return np.clip(rgb, 0, 255)

def lettering(w, h, lines, colour, fill_h=.8):
    """Stencil-painted text: rough edges, thin patches and short drips."""
    size = int(h * fill_h / max(1, sum(l[1] for l in lines)))
    mask = Image.new('L', (w, h), 0); d = ImageDraw.Draw(mask)
    y = (h - sum(int(size * l[1] * 1.08) for l in lines)) // 2
    for text, rel in lines:
        font = ImageFont.truetype(FONT, int(size * rel))
        while d.textlength(text, font=font) > w * .9:
            font = ImageFont.truetype(FONT, font.size - 4)
        x = (w - d.textlength(text, font=font)) / 2 + rng.uniform(-5, 5)
        d.text((x, y), text, fill=255, font=font)
        y += int(size * rel * 1.08)
    m = np.asarray(mask.filter(ImageFilter.GaussianBlur(1.4)), dtype=float) / 255
    m = np.clip((m + noise(w, h, 4, .34) - .17 - .25) * 3.2, 0, 1)
    wear = np.clip(noise(w, h, 28, 1.25) - .2, 0, 1)
    m *= .55 + .45 * wear
    for _ in range(max(3, w // 90)):           # drips below painted strokes
        x = int(rng.integers(0, w - 3)); ys = np.nonzero(m[:, x] > .5)[0]
        if len(ys):
            y0 = int(ys.max()); n = int(rng.integers(8, 42))
            m[y0:min(h, y0 + n), x:x + 2] = np.maximum(m[y0:min(h, y0 + n), x:x + 2], .75)
    return np.stack([np.full((h, w), c) for c in colour], 2), m[..., None]

atlas = np.zeros((SIZE, SIZE, 3)); height = np.zeros((SIZE, SIZE)); gloss = np.full((SIZE, SIZE), 18.0); glow = np.zeros((SIZE, SIZE, 3))

def panel(name, kind, lines=None, colour=(226, 214, 176), fill_h=.8):
    x, y, w, h = PANELS[name]
    rgb = board(w, h, kind)
    height[y:y + h, x:x + w] = noise(w, h, 3, .5) + noise(w, h, 24, .5)
    gloss[y:y + h, x:x + w] = 34 if kind == 'steel' else 14
    if lines:
        ink, m = lettering(w, h, lines, colour, fill_h)
        rgb = rgb * (1 - m) + ink * m
        height[y:y + h, x:x + w] += m[..., 0] * .5
        gloss[y:y + h, x:x + w] += m[..., 0] * 18
    rgb *= 1 - np.clip(noise(w, h, 60, 1.1) - .55, 0, .4)[..., None]     # grime
    atlas[y:y + h, x:x + w] = np.clip(rgb, 0, 255)

panel('cash', 'plywood', [('CAPS', 1.25), ('UP FRONT', .85), ('NO NAMES', .8)], (226, 214, 176))
panel('knock', 'plywood', [('CLOSED', 1.0), ('FOR', .5), ('SURGERY', .9), ('KNOCK ONLY', .55), ('IF NECESSARY', .5)], (242, 86, 62))
panel('jars', 'plywood', [('DO NOT', .9), ('TOUCH', 1.15), ('THE JARS', .9)], (226, 214, 176))
panel('still', 'steel', [('IF YOU CAN READ THIS', .72), ('HOLD STILL', 1.15)], (226, 196, 60))
panel('sharps', 'steel', [('SHARPS', 1.2), ('BIN IS UNDER THE TABLE', .46)], (242, 86, 62))
panel('edge', 'plywood')

# her mark: a stitched eye, painted dark on steel with a lit teal line
x, y, w, h = PANELS['logo']
atlas[y:y + h, x:x + w] = np.clip(board(w, h, 'steel') * .42, 0, 255)
height[y:y + h, x:x + w] = noise(w, h, 3, .5); gloss[y:y + h, x:x + w] = 30
line = Image.new('L', (w, h), 0); d = ImageDraw.Draw(line)
cx, cy = w / 2, h / 2; rx, ry = w * .38, h * .22
top = [(cx + rx * t, cy - ry * (1 - t * t)) for t in np.linspace(-1, 1, 64)]
bot = [(cx + rx * t, cy + ry * (1 - t * t)) for t in np.linspace(-1, 1, 64)]
d.line(top, fill=255, width=11); d.line(bot, fill=255, width=11)
d.ellipse((cx - 48, cy - 48, cx + 48, cy + 48), outline=255, width=11)
for t in np.linspace(-.72, .72, 7):                     # the stitches along the lower lid
    px, py = cx + rx * t, cy + ry * (1 - t * t)
    d.line([(px - 7, py - 22), (px + 7, py + 22)], fill=255, width=7)
line = np.asarray(line, dtype=float) / 255
halo = np.asarray(Image.fromarray((line * 255).astype('uint8')).filter(ImageFilter.GaussianBlur(11)), dtype=float) / 255
teal = np.array([60, 235, 215], dtype=float)
atlas[y:y + h, x:x + w] = np.clip(atlas[y:y + h, x:x + w] * (1 - line[..., None]) + teal * .55 * line[..., None], 0, 255)
glow[y:y + h, x:x + w] = np.clip(teal * (line + halo * .28)[..., None], 0, 255)

# normal map from the painted height
gy, gx = np.gradient(height)
n = np.dstack([-gx * 2.2, -gy * 2.2, np.ones_like(height)]); n /= np.linalg.norm(n, axis=2, keepdims=True)
nm = ((n * .5 + .5) * 255).astype('uint8')
os.makedirs(OUT, exist_ok=True)
Image.fromarray(atlas.astype('uint8')).save(os.path.join(OUT, 'CWC_Signs_d.png'))
Image.fromarray(nm).save(os.path.join(OUT, 'CWC_Signs_n.png'))
Image.fromarray(np.dstack([np.clip(gloss * 1.6, 0, 255), np.clip(gloss, 0, 255), np.zeros_like(gloss)]).astype('uint8')).save(os.path.join(OUT, 'CWC_Signs_s.png'))
Image.fromarray(glow.astype('uint8')).save(os.path.join(OUT, 'CWC_Signs_g.png'))
print('SIGNS OK', OUT)
