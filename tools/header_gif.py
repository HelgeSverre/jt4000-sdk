"""Draw the README header: the sound box's orbit scene on a wide virtual OLED, with a title.

Uses the SDK's own 8x8 font and the same orbit maths as sdk/examples/ambient (draw_motion).

    uv run --with pillow tools/header_gif.py            # writes docs/header.gif
"""

from __future__ import annotations

import math
import re
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
W, H, SCALE = 224, 56, 4  # logical pixels, and screen pixels per logical pixel
FRAMES, TRAIL = 60, 14
TITLE = "JT-4000 CUSTOM FIRMWARE SDK"
BG, OFF, ON = (10, 12, 16), (20, 24, 30), (214, 236, 255)


def font() -> list[list[int]]:
    text = (ROOT / "sdk/third_party/font8x8_basic.h").read_text()
    rows = re.findall(r"\{\s*((?:0x[0-9A-Fa-f]{2},\s*){7}0x[0-9A-Fa-f]{2})\s*\}", text)
    return [[int(v, 16) for v in r.replace(" ", "").split(",")] for r in rows]


def frame(n: int, glyphs: list[list[int]]) -> Image.Image:
    lit: set[tuple[int, int]] = set()

    def block(x: int, y: int, w: int, h: int) -> None:
        lit.update((x + i, y + j) for i in range(w) for j in range(h))

    x0 = (W - 8 * len(TITLE)) // 2
    for i, ch in enumerate(TITLE):
        for r, bits in enumerate(glyphs[ord(ch)]):
            lit.update((x0 + 8 * i + c, 4 + r) for c in range(8) if bits >> c & 1)

    cx, cy, rx, ry = W // 2, 35, 92, 15

    def at(angle: float) -> tuple[int, int]:
        return cx + int(rx * math.cos(2 * math.pi * angle)), cy + int(
            ry * math.sin(2 * math.pi * angle)
        )

    for i in range(0, 96, 2):  # the orbit, dotted
        lit.add(at(i / 96))
    for k in range(TRAIL * 6):  # where the dot has just been
        lit.add(at((n - k / 6) / FRAMES))
    hx, hy = at(n / FRAMES)
    block(hx - 2, hy - 2, 5, 5)
    block(cx - 1, cy - 1, 2, 2)

    img = Image.new("RGB", (W * SCALE, H * SCALE), BG)
    d = ImageDraw.Draw(img)
    for y in range(H):
        for x in range(W):
            colour = ON if (x, y) in lit else OFF
            d.rectangle(
                [x * SCALE, y * SCALE, x * SCALE + SCALE - 2, y * SCALE + SCALE - 2],
                fill=colour,
            )
    return img


def main() -> None:
    glyphs = font()
    frames = [frame(n, glyphs).quantize(colors=8) for n in range(FRAMES)]
    out = ROOT / "docs/header.gif"
    frames[0].save(
        out, save_all=True, append_images=frames[1:], duration=40, loop=0, optimize=True
    )
    print(
        f"wrote {out}: {W * SCALE}x{H * SCALE}, {FRAMES} frames, {out.stat().st_size // 1024} KiB"
    )


if __name__ == "__main__":
    main()
