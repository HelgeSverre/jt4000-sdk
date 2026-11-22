"""Draw the README header: the display of examples/demo, as a GIF.

Runs the demo firmware built for this computer (`make host`), takes a few seconds from
several of its scenes and draws each display pixel as a small square.

    uv run --with pillow --with pygame-ce tools/header_gif.py      # writes docs/header.gif
"""

from __future__ import annotations

import ctypes
from pathlib import Path

import cli
import panel

try:
    from PIL import Image
except ModuleNotFoundError:
    raise SystemExit(
        "error: needs Pillow. Run: uv run --with pillow --with pygame-ce tools/header_gif.py"
    ) from None

ROOT = Path(__file__).resolve().parent.parent
W, H, SCALE, FPS = 128, 32, 6, 20
BAR_MS = 1875
# (first bar, bars) of the demo to show: name, cube, plasma, shooter, drums, address, tube, twister
CUTS = [
    (1, 1.6),
    (5, 1.6),
    (13, 1.2),
    (28, 2.4),
    (32, 2),
    (36.2, 1.6),
    (44, 1.2),
    (48, 1.2),
]
BG, OFF, ON = (8, 10, 14), (16, 20, 26), (120, 200, 255)


def picture(px) -> Image.Image:
    img = Image.new("RGB", (W * SCALE, H * SCALE), BG)
    cell = Image.new("RGB", (SCALE - 1, SCALE - 1))
    for y in range(H):
        for x in range(W):
            cell.paste(ON if px[y * W + x] else OFF, (0, 0, SCALE - 1, SCALE - 1))
            img.paste(cell, (x * SCALE, y * SCALE))
    return img


def main() -> None:
    dll = panel.boot(panel.build("demo"))
    dll.jt_host_strict(1)
    dll.jt_host_input(0, 0, 0, 2000, 2000, ctypes.c_int32(0))
    px = (ctypes.c_uint8 * (W * H))()
    buf = (ctypes.c_int16 * 64)()
    wanted = sorted(
        round((bar * BAR_MS + i * 1000 / FPS) / 2)
        for bar, bars in CUTS
        for i in range(int(bars * BAR_MS * FPS / 1000))
    )
    frames = []
    for block in range(wanted[-1] + 1):  # one block of sound is 2 ms
        dll.jt_host_audio(buf, 64)
        if block == wanted[len(frames)]:
            dll.jt_host_display(px)
            frames.append(picture(px).quantize(colors=4))
    out = ROOT / "docs/header.gif"
    frames[0].save(
        out,
        save_all=True,
        append_images=frames[1:],
        duration=1000 // FPS,
        loop=0,
        optimize=True,
    )
    print(
        f"wrote {out}: {W * SCALE}x{H * SCALE}, {len(frames)} frames, {out.stat().st_size // 1024} KiB"
    )


if __name__ == "__main__":
    cli.run(main)
