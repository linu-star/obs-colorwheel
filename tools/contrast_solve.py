def lum(h):
    def lin(c):
        c /= 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * lin((h >> 16) & 255) + 0.7152 * lin((h >> 8) & 255) + 0.0722 * lin(h & 255)


def ratio(fg, bg):
    a, b = lum(fg), lum(bg)
    hi, lo = max(a, b), min(a, b)
    return (hi + 0.05) / (lo + 0.05)


BG = 0x1B1D21
TRACK = 0x2A2D33

print("== 底部提示文字 (背景 #1B1D21, 目标 >= 4.5) ==")
for c in [0x6E747E, 0x7A8089, 0x858B96, 0x8A909B, 0x9097A2, 0x98A0AB]:
    print(f"  #{c:06X}  {ratio(c, BG):.2f}  {'OK' if ratio(c, BG) >= 4.5 else '-'}")

print("\n== 置顶按钮：白字 #E8EBF0 在蓝底上 (目标 >= 4.5) ==")
for c in [0x476BD7, 0x4265D0, 0x3D5FC8, 0x3858BE, 0x3352B4, 0x2E4CAA]:
    print(f"  #{c:06X}  {ratio(0xE8EBF0, c):.2f}  {'OK' if ratio(0xE8EBF0, c) >= 4.5 else '-'}")

print("\n== 滑杆已填充部分 vs 轨道 #2A2D33 (目标 >= 3.0) ==")
for c in [0x476BD7, 0x5074DC, 0x5A7DE2, 0x6386E8, 0x6D8FEE]:
    print(f"  #{c:06X}  {ratio(c, TRACK):.2f}  {'OK' if ratio(c, TRACK) >= 3.0 else '-'}")

print("\n== 滑杆轨道 vs 窗口背景 #1B1D21 (目标 >= 3.0) ==")
for c in [0x2A2D33, 0x33373F, 0x3A3F49, 0x414651, 0x484E59, 0x4F5560]:
    print(f"  #{c:06X}  {ratio(c, BG):.2f}  {'OK' if ratio(c, BG) >= 3.0 else '-'}")

print("\n== 组合校验（同时满足）==")
hint, accent, fill, track = 0x8A909B, 0x3D5FC8, 0x6386E8, 0x414651
print(f"  提示 #8A909B on #1B1D21        = {ratio(hint, BG):.2f}  (>=4.5)")
print(f"  白字 #E8EBF0 on accent #3D5FC8 = {ratio(0xE8EBF0, accent):.2f}  (>=4.5)")
print(f"  填充 #6386E8 on track #414651  = {ratio(fill, track):.2f}  (>=3.0)")
print(f"  轨道 #414651 on bg #1B1D21     = {ratio(track, BG):.2f}  (>=3.0)")
print(f"  旋钮 #E1E4EA on track #414651  = {ratio(0xE1E4EA, track):.2f}  (>=3.0)")
