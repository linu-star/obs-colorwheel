"""Generate a colour-grading test chart for checking the OBS primary-grade filter.

Layout is chosen so that each control has an unambiguous target:
  * a continuous grey ramp      -> banding and contrast shape
  * stepped grey patches        -> neutral tracking, lift/gain endpoints
  * saturated primaries         -> saturation / hue, and the gamma channel coupling
  * skin tones                  -> the usual sanity check
  * a near-neutral ramp         -> colour casts are obvious here
"""
from PIL import Image, ImageDraw, ImageFont

W, H = 1920, 1080
img = Image.new("RGB", (W, H), (18, 18, 18))
d = ImageDraw.Draw(img)

try:
    font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 30)
    font_s = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 22)
except Exception:
    font = ImageFont.load_default()
    font_s = font

def label(x, y, text, fill=(200, 200, 200), f=None):
    d.text((x, y), text, font=f or font_s, fill=fill)

# ---------------------------------------------------------------- 1. bars
BARS = [
    ("白", (235, 235, 235)), ("黄", (235, 235, 16)), ("青", (16, 235, 235)),
    ("绿", (16, 235, 16)), ("品红", (235, 16, 235)), ("红", (235, 16, 16)),
    ("蓝", (16, 16, 235)), ("黑", (16, 16, 16)),
]
bw = W // len(BARS)
for i, (name, c) in enumerate(BARS):
    d.rectangle([i * bw, 0, (i + 1) * bw - 1, 240], fill=c)
    label(i * bw + 12, 250, name, (170, 170, 170))

# ------------------------------------------------ 2. continuous grey ramp
y0 = 290
for x in range(W):
    v = round(x * 255 / (W - 1))
    d.line([(x, y0), (x, y0 + 120)], fill=(v, v, v))
label(12, y0 + 130, "连续灰阶 0→255（看断层/色带）")

# ------------------------------------------------------- 3. stepped grey
y0 = 460
steps = 11
for i in range(steps):
    v = round(i * 255 / (steps - 1))
    x0 = i * W // steps
    x1 = (i + 1) * W // steps - 1
    d.rectangle([x0, y0, x1, y0 + 100], fill=(v, v, v))
    label(x0 + 8, y0 + 108, str(v), (150, 150, 150))
label(12, y0 + 145, "阶梯灰阶（看中性是否偏移）")

# ------------------------------------------------------ 4. saturated ramp
y0 = 620
# 100% and 50% primaries / secondaries
CH = [
    ("R", (255, 0, 0)), ("G", (0, 255, 0)), ("B", (0, 0, 255)),
    ("C", (0, 255, 255)), ("M", (255, 0, 255)), ("Y", (255, 255, 0)),
]
cw = W // 12
for i, (n, c) in enumerate(CH):
    d.rectangle([i * cw, y0, (i + 1) * cw - 1, y0 + 110], fill=c)
    c50 = tuple(int(v * 0.5) for v in c)
    d.rectangle([i * cw, y0 + 110, (i + 1) * cw - 1, y0 + 220], fill=c50)
    label(i * cw + 8, y0 + 226, n, (180, 180, 180))

# skin tones occupy the remaining right half
SKIN = [
    ("浅", (241, 194, 160)), ("中", (224, 172, 133)),
    ("深", (198, 134, 96)), ("暗", (141, 85, 60)),
]
for i, (n, c) in enumerate(SKIN):
    x0 = 6 * cw + i * cw
    d.rectangle([x0, y0, x0 + cw - 1, y0 + 220], fill=c)
    label(x0 + 8, y0 + 226, n, (180, 180, 180))
label(12, y0 + 255, "饱和原色 / 肤色（推色轮、调饱和度都看这里）")

# -------------------------------------------------- 5. near-neutral ramp
y0 = 900
for x in range(W):
    t = x / (W - 1)
    # 180 -> 255 with a slight hue drift, so any cast shows immediately
    r = int(180 + 75 * t)
    g = int(184 + 71 * t)
    b = int(188 + 67 * t)
    d.line([(x, y0), (x, y0 + 90)], fill=(r, g, b))
label(12, y0 + 98, "近中性渐变（任何色偏在这里一眼就能看出来）")

# header strip
d.rectangle([0, H - 60, W, H], fill=(28, 28, 28))
label(14, H - 52, "obs-colorwheel 测试图  ·  1920×1080", (120, 200, 255), font)

out = r"C:\Users\qiusen\.dsh\dsh_wechat\obs-colorwheel\assets\colorwheel_testchart.png"
import os
os.makedirs(os.path.dirname(out), exist_ok=True)
img.save(out, "PNG")
print(out, os.path.getsize(out), "bytes")
