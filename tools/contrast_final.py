def lum(h):
    def lin(c):
        c /= 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * lin((h >> 16) & 255) + 0.7152 * lin((h >> 8) & 255) + 0.0722 * lin(h & 255)


def r(f, b):
    a, c = lum(f), lum(b)
    hi, lo = max(a, c), min(a, c)
    return (hi + 0.05) / (lo + 0.05)


BG = 0x1B1D21
TRACK = 0x3A3F49
FILL = 0x6D8FEE
ACC = 0x3D5FC8
HINT = 0x8A909B

print("final token set")
print(f"  hint   #8A909B on bg       {r(HINT, BG):.2f}   need 4.5")
print(f"  white  #E8EBF0 on accent   {r(0xE8EBF0, ACC):.2f}   need 4.5")
print(f"  fill   #6D8FEE on track    {r(FILL, TRACK):.2f}   need 3.0")
print(f"  track  #3A3F49 on bg       {r(TRACK, BG):.2f}   need 3.0")
print(f"  knob   #E1E4EA on track    {r(0xE1E4EA, TRACK):.2f}   need 3.0")
print()
print("how bright must the track be to reach 3.0 against bg?")
for c in [0x5A606B, 0x6A707C, 0x7A808C, 0x8A909C, 0x9AA0AC]:
    print(f"  #{c:06X} vs bg {r(c, BG):.2f}")
