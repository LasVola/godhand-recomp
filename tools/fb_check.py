"""Count lit pixels (any RGB channel > 16) in a CT32/CT24 framebuffer of a GS VRAM dump.

usage: python tools/fb_check.py <vram.bin> <block base hex> <fbw> <width> <height>
"""
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from gs_vram import read32  # noqa: E402

vram = open(sys.argv[1], "rb").read()
base, bw, w, h = int(sys.argv[2], 16), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
lit = 0
for y in range(0, h, 2):
    for x in range(0, w, 2):
        c = read32(vram, base, bw, x, y)
        if (c & 0xFF) > 16 or ((c >> 8) & 0xFF) > 16 or ((c >> 16) & 0xFF) > 16:
            lit += 1
print(f"lit pixels (sampled 1/4): {lit}")
