def lum(h):
    def lin(c):
        c /= 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * lin((h >> 16) & 255) + 0.7152 * lin((h >> 8) & 255) + 0.0722 * lin(h & 255)


def ratio(fg, bg):
    a, b = lum(fg), lum(bg)
    hi, lo = max(a, b), min(a, b)
    return (hi + 0.05) / (lo + 0.05)


# name, fg, bg, kind ("text" -> 4.5, "ui" -> 3.0)
PAIRS = [
    ("标题文字", 0xE6E8EC, 0x24262B, "text"),
    ("色轮标签", 0xE6E8EC, 0x1B1D21, "text"),
    ("通道读数", 0xB9BEC6, 0x1B1D21, "text"),
    ("主读数", 0xE1E4EA, 0x1B1D21, "text"),
    ("底部提示", 0x8A909B, 0x1B1D21, "text"),
    ("关闭字形", 0xC8CCD4, 0x3A3E46, "text"),
    ("关闭 hover", 0xC8CCD4, 0x474C56, "text"),
    ("关闭 press", 0xC8CCD4, 0x2E323A, "text"),
    ("置顶字形", 0xE8EBF0, 0x3D5FC8, "text"),
    ("置顶 hover", 0xE8EBF0, 0x3550AE, "text"),
    ("置顶 press", 0xE8EBF0, 0x2E4699, "text"),
    ("滑杆填充/轨道", 0x6D8FEE, 0x3A3F49, "ui"),
    ("滑杆旋钮/轨道", 0xE1E4EA, 0x3A3F49, "ui"),
    ("轨道/窗口背景", 0x3A3F49, 0x1B1D21, "ui"),
    ("焦点环/面板", 0x6D8FEE, 0x24262B, "ui"),
]

fails = 0
print(f"{'element':<16}{'fg':>9}{'bg':>9}{'ratio':>7}{'need':>6}  verdict")
print("-" * 60)
for name, fg, bg, kind in PAIRS:
    r = ratio(fg, bg)
    need = 4.5 if kind == "text" else 3.0
    ok = r >= need
    if not ok:
        fails += 1
    print(f"{name:<16}{'#%06X' % fg:>9}{'#%06X' % bg:>9}{r:>7.2f}{need:>6.1f}  {'PASS' if ok else 'FAIL'}")

print()
print(f"{len(PAIRS) - fails}/{len(PAIRS)} pass")
print("UI/UX Pro Max rule: text 4.5:1 (WCAG AA), component graphics 3:1.")
