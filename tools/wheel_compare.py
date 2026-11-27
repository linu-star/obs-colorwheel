"""Render the current HSV wheel next to a Cb/Cr (chroma-plane) wheel.

DaVinci Resolve does not place hues at even 120 degree spacing: its wheel is a
chroma-plane plot, so yellow gets a narrow wedge and cyan/green a wide one.
These two images make the difference visible side by side.
"""
import math

from PIL import Image, ImageDraw

SIZE = 300
RIM = 0.83  # disc radius as a fraction of half-size


def hsv_to_rgb(h, s, v):
    h = h - math.floor(h)
    i = int(h * 6.0)
    f = h * 6.0 - i
    p = v * (1.0 - s)
    q = v * (1.0 - s * f)
    t = v * (1.0 - s * (1.0 - f))
    i %= 6
    if i == 0:
        return v, t, p
    if i == 1:
        return q, v, p
    if i == 2:
        return p, v, t
    if i == 3:
        return p, q, v
    if i == 4:
        return t, p, v
    return v, p, q


def rgb_to_hue(r, g, b):
    mx = max(r, g, b)
    mn = min(r, g, b)
    d = mx - mn
    if d < 1e-9:
        return 0.0
    if mx == r:
        h = ((g - b) / d) % 6.0
    elif mx == g:
        h = (b - r) / d + 2.0
    else:
        h = (r - g) / d + 4.0
    return h / 6.0


def chroma_delta(cb, cr):
    """YCbCr -> RGB offsets with Y held at zero (Rec.709 coefficients)."""
    dr = 1.5748 * cr
    db = 1.8556 * cb
    dg = -(0.4681 * cr + 0.1873 * cb)
    return dr, dg, db


def build(kind, size=SIZE):
    img = Image.new("RGB", (size, size), (27, 29, 33))
    px = img.load()
    c = size / 2.0
    r_disc = c * RIM
    for y in range(size):
        for x in range(size):
            dx = x - c + 0.5
            dy = y - c + 0.5
            r = math.hypot(dx, dy)
            if r > r_disc:
                continue
            s = min(1.0, r / r_disc)
            if kind == "hsv":
                h = math.atan2(-dy, dx) / (2 * math.pi)
            else:
                # screen (x right, y down) -> chroma plane (Cb right, Cr up)
                cb, cr = dx, -dy
                dr, dg, db = chroma_delta(cb, cr)
                h = rgb_to_hue(dr, dg, db)
            rr, gg, bb = hsv_to_rgb(h, s, 0.96)
            px[x, y] = (int(rr * 255), int(gg * 255), int(bb * 255))
    return img


def mark(img, angles, labels):
    d = ImageDraw.Draw(img)
    c = img.size[0] / 2.0
    rad = c * RIM * 0.72
    for ang, lab in zip(angles, labels):
        x = c + math.cos(math.radians(ang)) * rad
        y = c - math.sin(math.radians(ang)) * rad
        d.ellipse([x - 3, y - 3, x + 3, y + 3], fill=(255, 255, 255))
        d.text((x + 5, y - 5), lab, fill=(255, 255, 255))


def prime_angles(kind):
    """Screen angle (degrees, CCW, 0 = right) where R, G, B peak."""
    out = []
    for name in ("R", "G", "B"):
        best, bestv = 0, -1e9
        for i in range(720):
            ang = i * 0.5
            dx = math.cos(math.radians(ang))
            dy = -math.sin(math.radians(ang))
            if kind == "hsv":
                th = math.atan2(-dy, dx)
                ph = {"R": 0.0, "G": 2 * math.pi / 3, "B": 4 * math.pi / 3}[name]
                v = math.cos(th - ph)
            else:
                dr, dg, db = chroma_delta(dx, -dy)
                v = {"R": dr, "G": dg, "B": db}[name]
            if v > bestv:
                bestv, best = v, ang
        out.append((name, best))
    return out


left = build("hsv")
right = build("chroma")
for img, kind in ((left, "hsv"), (right, "chroma")):
    angles = prime_angles(kind)
    mark(img, [a for _, a in angles], [n for n, _ in angles])
    print(kind, [(n, round(a, 1)) for n, a in angles])

W = SIZE * 2 + 40
canvas = Image.new("RGB", (W, SIZE + 60), (20, 22, 26))
canvas.paste(left, (10, 40))
canvas.paste(right, (SIZE + 30, 40))
d = ImageDraw.Draw(canvas)
d.text((10, 14), "current: uniform HSV wheel", fill=(200, 205, 212))
d.text((SIZE + 30, 14), "candidate: chroma-plane (Resolve-like)", fill=(200, 205, 212))
d.text((10, SIZE + 44), "dot + letter = where R / G / B peak", fill=(140, 146, 156))
canvas.save("C:/Users/qiusen/.dsh/dsh_wechat/obs-colorwheel/tools/design/wheel_compare.png")
print("saved")
