"""A JT-4000 on the screen: run a firmware in a window that looks like the synth.

The firmware's own C source is compiled for this computer (`make host`) and runs at full
speed with sound. Click and drag the controls, take screenshots, record video. It is for
playing, demos and pictures; tools/emu.py runs the real ARM binary and is the one to trust
about timing and hardware. The analog filter here is a digital stand-in.

    uv run --with pygame-ce --with sounddevice --with numpy tools/panel.py speak
    uv run --with pygame-ce --with numpy tools/panel.py groove --ms 8000 --video groove.mp4
    uv run --with pygame-ce --with numpy tools/panel.py drone --ms 3000 --script "1500 tap=lfo; 2500 shot"

Mouse: click a button or key, drag a knob up and down or scroll over it. Click SELECT to
press it, drag or scroll to turn it.
Keyboard: 1-6 buttons, space SELECT, left/right turn SELECT, z x c v b n m , . / and
d f h j k ; the touch keys, P screenshot, R start/stop a video, Esc quit.
The power switch (click it, or O) turns the synth off, and on again from the start.
Two hands: shift + click (or right-click), or shift + a key, leaves that control held down
until you do it again; 0 lets go of everything. So "hold AMP and turn SELECT" is shift+4,
then scroll over SELECT. A plain key also works as a hold while the mouse does the rest.
"""

from __future__ import annotations

import argparse
import ctypes
import math
import os
import shutil
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import cli

ROOT = Path(__file__).resolve().parent.parent
RATE, BLOCK = 32000, 64
W, H = 1000, 720  # the window; positions below are in these units
Z = 2  # the picture is drawn this many times larger, so the display's pixels come out whole
BUTTONS = ["osc", "lfo", "filter", "amp", "arp", "settings"]
KEY_BITS = {
    "lfo": 1,
    "filter": 2,
    "amp": 4,
    "arp": 8,
    "select": 0x20,
}  # the key chip's byte
WHITE = [0, 1, 3, 5, 6, 8, 10, 12, 13, 15]  # touch keys B C D E F G A B C D
BLACK = {
    2: 1,
    4: 2,
    7: 4,
    9: 5,
    11: 6,
    14: 8,
}  # sharps: key -> the white key to their left
TYPING = "zxcvbnm,./"  # white keys
TYPING_BLACK = {"d": 2, "f": 4, "h": 7, "j": 9, "k": 11, ";": 14}

# colours taken from the instrument: blue panel, orange legends, red button lights
BODY, BLUE, SHELF, LINE = (38, 40, 46), (66, 84, 126), (48, 62, 96), (27, 29, 36)
ORANGE, INK, PIXEL, GLASS = (
    (246, 152, 42),
    (224, 228, 236),
    (64, 184, 255),
    (10, 16, 26),
)
RED, UNLIT, KEY_INK = (255, 56, 56), (92, 110, 156), (232, 234, 238)


def half(*v: float):
    """Reference-photograph coordinates (2000 wide) to window coordinates."""
    return tuple(round(x / 2) for x in v) if len(v) > 1 else round(v[0] / 2)


def build(app: str) -> Path:
    """Compile the firmware for this computer; returns the library."""
    ext = "dylib" if sys.platform == "darwin" else "so"
    lib = ROOT / "sdk/build/host" / f"{app}.{ext}"
    if not (ROOT / "sdk/examples" / app / "main.c").is_file():
        names = sorted(p.name for p in (ROOT / "sdk/examples").iterdir() if p.is_dir())
        sys.exit(f"error: no firmware called '{app}'. There are: {', '.join(names)}")
    r = subprocess.run(
        ["make", "-C", str(ROOT / "sdk"), "host", f"APP={app}"],
        check=False,
        capture_output=True,
        text=True,
    )
    if r.returncode:
        sys.exit(
            f"error: could not build {app} for this computer:\n{r.stdout}{r.stderr}"
        )
    return lib


def boot(lib: Path) -> ctypes.CDLL:
    """Load the firmware and start it, from the beginning.

    Each boot loads its own copy of the library: that is what gives the firmware fresh
    memory, as a power cycle does. (The copy before it is left behind, asleep.)"""
    copy = Path(tempfile.mkdtemp(prefix="jt4000-")) / lib.name
    shutil.copy(lib, copy)
    dll = ctypes.CDLL(str(copy))
    dll.jt_host_audio.argtypes = [ctypes.c_void_p, ctypes.c_int]
    dll.jt_host_display.argtypes = [ctypes.c_void_p]
    dll.jt_host_display.restype = ctypes.c_uint
    dll.jt_host_start()
    return dll


class Synth:
    """The state of the controls, and the picture of the device."""

    def __init__(self, lib: Path, app: str):
        import pygame

        self.pg, self.lib, self.app = pygame, lib, app
        self.dll: ctypes.CDLL | None = (
            None  # the running firmware; None while switched off
        )
        self.strict = True
        self.down: set[str] = set()  # buttons held (osc..settings, select)
        self.touch: set[int] = set()  # touch keys held
        self.latched: set[str | int] = set()  # held by right-click
        self.since: dict[str | int, int] = {}  # when each control went down, ms
        self.release_at: dict[str | int, int] = {}  # let go of it then
        self.knob = {"cutoff": 4095, "resonance": 0, "volume": 3000}
        self.enc = 0
        self.pixels = (ctypes.c_uint8 * (128 * 32))()
        self.oled = pygame.Surface((128, 32))
        self.small = pygame.font.Font(None, 19 * Z)
        self.legend = pygame.font.Font(None, 23 * Z)
        self.title = pygame.font.Font(None, 62 * Z)
        self.sub = pygame.font.Font(None, 34 * Z)
        r = pygame.Rect
        columns = [
            240,
            545,
            848,
            1152,
            1455,
            1760,
        ]  # the six buttons; SEL, A and B sit above 1, 5 and 6
        self.pos_button = {
            n: half(x, 636) for n, x in zip(BUTTONS, columns, strict=True)
        }
        self.pos_knob = {
            "select": half(240, 395),
            "cutoff": half(1455, 395),
            "resonance": half(1760, 395),
            "volume": half(1455, 150),
        }
        self.rect_oled = r(808, 348, 384, 96)  # in drawn pixels: 128 x 32, three each
        # touch keys: ten naturals, each narrow at the top where sharps sit beside it
        self.shape_key: dict[int, list[tuple[int, int]]] = {}
        self.rect_key: dict[int, list] = {}
        sharp_after = set(BLACK.values())
        for i, k in enumerate(WHITE):
            left, right = 70 + 186 * i, 70 + 186 * (i + 1)
            a = left + (62 if (i - 1) in sharp_after else 6)
            b = right - (62 if i in sharp_after else 6)
            pts = [
                (a, 946),
                (b, 946),
                (b, 1190),
                (right - 6, 1190),
                (right - 6, 1378),
                (left + 6, 1378),
                (left + 6, 1190),
                (a, 1190),
            ]
            self.shape_key[k] = [half(*pt) for pt in pts]
            self.rect_key[k] = [
                r(*half(a, 946), half(b - a), half(244)),
                r(*half(left + 6, 1190), half(174), half(188)),
            ]
        for k, left in BLACK.items():
            x = 70 + 186 * (left + 1)
            self.rect_key[k] = [r(*half(x - 48, 940), half(96), half(212))]

    # ---- input ----
    # The firmware reads its buttons every 8 ms and wants two readings in a row before it
    # believes one, as it must on the real hardware. A tap on a trackpad is over sooner
    # than that, so a press here always lasts at least HOLD_MS.
    HOLD_MS = 70

    def press(self, control: str | int, now: int) -> None:
        (self.touch if isinstance(control, int) else self.down).add(control)
        self.since[control] = now
        self.release_at.pop(control, None)
        self.push()

    def release(self, control: str | int, now: int) -> None:
        self.release_at[control] = max(now, self.since.get(control, 0) + self.HOLD_MS)

    def tick(self, now: int) -> None:
        for control, when in list(self.release_at.items()):
            if now >= when:
                (self.touch if isinstance(control, int) else self.down).discard(control)
                del self.release_at[control]
        self.push()

    def power(self, on: bool | None = None) -> None:
        """The power switch. Off: dark and silent. On: the firmware starts from the top."""
        on = self.dll is None if on is None else on
        if on and self.dll is None:
            dll = boot(self.lib)
            dll.jt_host_strict(int(self.strict))
            self.dll = dll
            self.push()
        elif not on:
            self.dll = None

    def push(self) -> None:
        if self.dll is None:
            return
        held = self.down | {x for x in self.latched if isinstance(x, str)}
        keys = self.touch | {x for x in self.latched if isinstance(x, int)}
        self.dll.jt_host_input(
            ("osc" in held) | ("settings" in held) << 1,
            sum(bit for name, bit in KEY_BITS.items() if name in held),
            sum(0x8000 >> k for k in keys),
            self.knob["cutoff"],
            self.knob["resonance"],
            ctypes.c_int32(self.enc),
        )

    def act(self, action: str) -> None:
        """One scripted input, in the words tools/emu.py uses."""
        name = action.lstrip("+-")
        if action.startswith("enc"):
            self.enc += int(action[3:])
        elif action.startswith("key="):
            self.touch = {int(k) for k in action[4:].split("+") if k}
        elif action.startswith(("cutoff=", "resonance=")):
            which, value = action.split("=")
            self.knob[which] = max(0, min(4095, int(value)))
        elif name in BUTTONS or name == "select":
            (self.down.add if action.startswith("+") else self.down.discard)(name)
        elif action in ("power", "off", "on"):
            self.power(None if action == "power" else action == "on")
        else:
            sys.exit(f"error: --script: unknown action '{action}'")
        self.push()

    def at(self, pos) -> str | int | None:
        """What is under the mouse: a button name, a knob name, or a touch key number."""
        for k in [*BLACK, *WHITE]:
            if any(rect.collidepoint(pos) for rect in self.rect_key[k]):
                return k
        for n, (x, y) in self.pos_button.items():
            if math.hypot(pos[0] - x, pos[1] - y) < 30:
                return n
        if 200 <= pos[0] <= 305 and 55 <= pos[1] <= 95:
            return "power"
        for n, (x, y) in self.pos_knob.items():
            if math.hypot(pos[0] - x, pos[1] - y) < 36:
                return n
        return None

    # ---- picture ----
    def d_rect(self, surface, colour, rect, width=0, border_radius=0) -> None:
        x, y, w, h = rect
        self.pg.draw.rect(
            surface,
            colour,
            (x * Z, y * Z, w * Z, h * Z),
            width * Z,
            border_radius=border_radius * Z,
        )

    def d_circle(self, surface, colour, center, radius, width=0) -> None:
        self.pg.draw.circle(
            surface, colour, (center[0] * Z, center[1] * Z), radius * Z, width * Z
        )

    def d_line(self, surface, colour, a, b, width=1) -> None:
        self.pg.draw.line(
            surface, colour, (a[0] * Z, a[1] * Z), (b[0] * Z, b[1] * Z), width * Z
        )

    def d_polygon(self, surface, colour, points, width=0) -> None:
        self.pg.draw.polygon(
            surface, colour, [(x * Z, y * Z) for x, y in points], width * Z
        )

    def text(
        self, surface, s: str, center, font=None, colour=ORANGE, tab=False
    ) -> None:
        img = (font or self.legend).render(s, True, colour)
        rect = img.get_rect(center=(center[0] * Z, center[1] * Z))
        if tab:  # a legend set into a frame line
            self.pg.draw.rect(
                surface, LINE, rect.inflate(22 * Z, 8 * Z), border_radius=9 * Z
            )
        surface.blit(img, rect)

    def draw_knob(self, surface, name: str, radius: int, ticks: bool) -> None:
        x, y = self.pos_knob[name]
        if name == "select":
            angle = self.enc * math.pi / 100  # about 200 counts per turn
        else:
            angle = (self.knob[name] / 4095 - 0.5) * 1.5 * math.pi
        if ticks:  # the scale printed around the filter knobs
            for i in range(21):
                a = (i / 20 - 0.5) * 1.5 * math.pi
                inner, outer = radius + 7, radius + (15 if i % 5 == 0 else 12)
                self.d_line(
                    surface,
                    INK,
                    (x + math.sin(a) * inner, y - math.cos(a) * inner),
                    (x + math.sin(a) * outer, y - math.cos(a) * outer),
                    2,
                )
        self.d_circle(surface, (30, 38, 60), (x, y + 4), radius + 3)
        self.d_circle(surface, (22, 23, 26), (x, y), radius)
        for i in range(36):  # the knurled edge
            a = angle + i * math.pi / 18
            self.d_line(
                surface,
                (52, 54, 60),
                (x + math.sin(a) * (radius - 3), y - math.cos(a) * (radius - 3)),
                (x + math.sin(a) * radius, y - math.cos(a) * radius),
                1,
            )
        self.d_circle(surface, (40, 42, 47), (x, y), radius - 5)
        self.d_circle(surface, (48, 50, 56), (x - 2, y - 3), radius - 11)
        if name != "select":  # the encoder has no mark; the pots have a white line
            tip = (
                x + math.sin(angle) * (radius - 4),
                y - math.cos(angle) * (radius - 4),
            )
            start = (
                x + math.sin(angle) * (radius - 15),
                y - math.cos(angle) * (radius - 15),
            )
            self.d_line(surface, (245, 246, 248), start, tip, 4)
        elif "select" in self.down or "select" in self.latched:
            self.d_circle(surface, ORANGE, (x, y), radius + 4, 2)

    def draw(self, surface) -> None:
        pg = self.pg
        surface.fill((0, 0, 0))
        self.d_rect(surface, BODY, (0, 0, W, H), border_radius=34)
        self.d_rect(
            surface, SHELF, (*half(28, 30), half(1944), half(900)), border_radius=26
        )
        self.d_rect(
            surface, BLUE, (*half(28, 30), half(1944), half(720)), border_radius=26
        )
        self.d_rect(surface, BLUE, (*half(28, 400), half(1944), half(330)))
        self.d_rect(surface, SHELF, (*half(28, 728), half(1944), half(150)))

        # top row: USB socket, power switch, name, volume, phones
        self.d_rect(
            surface,
            (14, 15, 18),
            (*half(168, 120), half(142), half(56)),
            border_radius=14,
        )
        self.d_rect(
            surface,
            (120, 130, 150),
            (*half(168, 120), half(142), half(56)),
            2,
            border_radius=14,
        )
        self.d_rect(
            surface,
            (40, 42, 50),
            (*half(190, 140), half(98), half(16)),
            border_radius=4,
        )
        self.d_rect(
            surface,
            (14, 15, 18),
            (*half(452, 124), half(126), half(52)),
            border_radius=5,
        )
        self.d_rect(
            surface,
            (44, 46, 52),
            (*half(520 if self.dll else 456, 128), half(54), half(44)),
            border_radius=4,
        )
        self.d_circle(surface, INK, half(420, 150), 6, 2)
        self.d_line(surface, INK, half(596, 136), half(596, 164), 2)
        name = self.title.render("JT-4000", True, ORANGE)
        surface.blit(name, name.get_rect(midleft=(676, 124)))
        micro = self.sub.render("MICRO", True, INK)
        surface.blit(
            micro,
            micro.get_rect(midleft=(676 + name.get_width() + 24, 132)),
        )
        self.text(
            surface,
            f"CUSTOM FIRMWARE:  {self.app.upper()}",
            half(1000, 190),
            self.small,
            INK,
        )
        self.text(surface, "VOLUME", half(1455, 62), colour=INK)
        self.text(surface, "PHONES", half(1760, 62), colour=INK)
        self.draw_knob(surface, "volume", 25, False)
        self.d_circle(surface, (16, 17, 20), half(1760, 150), 32)
        self.d_circle(surface, (150, 156, 168), half(1760, 150), 21, 4)
        self.d_circle(surface, (4, 4, 5), half(1760, 150), 15)

        # the framed section, with its legends set into the lines
        frame = pg.Rect(*half(100, 260), half(1800), half(440))
        self.d_rect(surface, LINE, frame, 5, border_radius=14)
        self.d_line(surface, LINE, half(104, 530), half(1896, 530), 5)
        self.text(surface, "SEL", half(240, 272), tab=True)
        self.text(surface, "A", half(1455, 272), tab=True)
        self.text(surface, "B", half(1760, 272), tab=True)
        self.text(surface, "ANALOG FILTER", half(1608, 308))
        self.text(surface, "ENTER", half(240, 495))
        self.text(surface, "CUTOFF", half(1455, 495))
        self.text(surface, "RESO", half(1760, 495))
        self.draw_knob(surface, "select", 25, False)
        self.draw_knob(surface, "cutoff", 25, True)
        self.draw_knob(surface, "resonance", 25, True)

        # display
        if self.dll:
            self.dll.jt_host_display(self.pixels)
        else:
            ctypes.memset(self.pixels, 0, len(self.pixels))
        px = pg.PixelArray(self.oled)
        on, off = self.oled.map_rgb(PIXEL), self.oled.map_rgb(GLASS)
        for y in range(32):
            row = self.pixels[y * 128 : y * 128 + 128]
            for x in range(128):
                px[x, y] = on if row[x] else off
        del px
        bezel = self.rect_oled.inflate(16, 40)
        pg.draw.rect(surface, GLASS, bezel)
        pg.draw.rect(surface, (40, 52, 80), bezel, 2)
        surface.blit(pg.transform.scale(self.oled, self.rect_oled.size), self.rect_oled)

        # buttons: round, red when lit
        leds = self.dll.jt_host_leds() if self.dll else 0
        labels = ["OSC-X", "LFO-X", "FILTER", "AMP", "ARP", "SETTINGS"]
        for i, n in enumerate(BUTTONS):
            x, y = self.pos_button[n]
            self.text(surface, labels[i], (x, half(543)), tab=True)
            held = n in self.down or n in self.latched
            lit = leds >> i & 1
            if lit:  # the light spills onto the panel
                glow = pg.Surface((120 * Z, 120 * Z), pg.SRCALPHA)
                for g in range(10):
                    self.d_circle(glow, (*RED, 6 + g * 3), (60, 60), 58 - g * 3)
                surface.blit(glow, ((x - 60) * Z, (y - 60) * Z))
            self.d_circle(surface, (30, 36, 56), (x, y + 2), 25)
            self.d_circle(
                surface, RED if lit else UNLIT, (x, y + (2 if held else 0)), 22
            )
            self.d_circle(
                surface,
                (255, 128, 120) if lit else (112, 130, 176),
                (x - 4, y - 5 + (2 if held else 0)),
                11,
            )
            if n in self.latched:
                self.d_circle(surface, INK, (x, y), 28, 2)

        # the touch keyboard: white outlines printed on glossy black
        self.d_rect(
            surface,
            (14, 14, 16),
            (*half(46, 924), half(1908), half(476)),
            border_radius=20,
        )
        for k in WHITE:
            touched = k in self.touch or k in self.latched
            if touched:
                self.d_polygon(surface, (40, 96, 150), self.shape_key[k])
            self.d_polygon(surface, PIXEL if touched else KEY_INK, self.shape_key[k], 2)
            if k in self.latched:
                self.d_circle(
                    surface,
                    ORANGE,
                    (self.rect_key[k][1].centerx, self.rect_key[k][1].bottom - 14),
                    5,
                )
        for k in BLACK:
            rect = self.rect_key[k][0]
            touched = k in self.touch or k in self.latched
            self.d_rect(
                surface,
                (40, 96, 150) if touched else (22, 22, 25),
                rect,
                border_radius=5,
            )
            inner = rect.inflate(-10, -10)
            step = 11
            for j in range(
                inner.x - inner.h, inner.right, step
            ):  # the lattice printed on the sharps
                for sign in (1, -1):
                    x0, x1 = j, j + inner.h
                    y0, y1 = (
                        (inner.y, inner.bottom)
                        if sign == 1
                        else (inner.bottom, inner.y)
                    )
                    lo, hi = max(x0, inner.x), min(x1, inner.right)
                    if lo < hi:
                        ya = y0 + (y1 - y0) * (lo - x0) / inner.h
                        yb = y0 + (y1 - y0) * (hi - x0) / inner.h
                        self.d_line(
                            surface,
                            PIXEL if touched else KEY_INK,
                            (lo, ya),
                            (hi, yb),
                            2,
                        )
            self.d_rect(
                surface, PIXEL if touched else KEY_INK, rect, 2, border_radius=5
            )
            if k in self.latched:
                self.d_circle(surface, ORANGE, (rect.centerx, rect.bottom + 10), 5)


def parse(script: str) -> list[tuple[int, str]]:
    events = []
    for item in filter(None, (x.strip() for x in script.split(";"))):
        try:
            when, action = item.split()
            at = int(when)
        except ValueError:
            sys.exit(f"error: --script: '{item}' is not 'ms action'")
        if action.startswith("tap="):
            events += [(at, "+" + action[4:]), (at + 80, "-" + action[4:])]
        else:
            events.append((at, action))
    return sorted(events, key=lambda e: e[0])


def write_wav(path: Path, pcm: bytes) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)


def encode(frames_raw: Path, audio: bytes, fps: int, out: Path) -> None:
    """Raw RGB frames plus sound -> mp4, or -> gif (no sound) by the file's ending."""
    if not shutil.which("ffmpeg"):
        sys.exit("error: making a video needs ffmpeg (brew install ffmpeg)")
    video = [
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "-s",
        f"{W}x{H}",
        "-r",
        str(fps),
        "-i",
        str(frames_raw),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        if out.suffix == ".gif":
            cmd = [
                *video,
                "-vf",
                "fps=20,scale=720:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=64[p];[b][p]paletteuse",
            ]
        else:
            wav = Path(tmp) / "audio.wav"
            write_wav(wav, audio)
            cmd = [
                *video,
                "-i",
                str(wav),
                "-c:v",
                "libx264",
                "-pix_fmt",
                "yuv420p",
                "-crf",
                "17",
                "-c:a",
                "aac",
                "-b:a",
                "192k",
                "-shortest",
            ]
        r = subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", *cmd, str(out)],
            check=False,
            capture_output=True,
            text=True,
        )
        if r.returncode:
            sys.exit(f"error: ffmpeg: {r.stderr.strip()}")


def render(synth: Synth, a: argparse.Namespace) -> None:
    """No window: run for a set time, as fast as it goes, and save pictures, sound, video."""
    pg = synth.pg
    surface = pg.Surface((W * Z, H * Z))
    events = parse(a.script)
    buf = (ctypes.c_int16 * BLOCK)()
    audio = bytearray()
    fps, shots = 30, 0
    raw = tempfile.NamedTemporaryFile(suffix=".rgb", delete=False) if a.video else None  # noqa: SIM115
    next_frame = 0.0
    for block in range(a.ms * RATE // 1000 // BLOCK):
        ms = block * BLOCK * 1000 // RATE
        while events and events[0][0] <= ms:
            action = events.pop(0)[1]
            if action == "shot":
                synth.draw(surface)
                shots += 1
                out = Path(f"{a.shot_prefix or a.app}-{ms}.png")
                pg.image.save(surface, str(out))
                print(f"wrote {out}")
            else:
                synth.act(action)
        if synth.dll:
            synth.dll.jt_host_audio(buf, BLOCK)
        else:
            ctypes.memset(buf, 0, ctypes.sizeof(buf))
        audio += bytes(buf)
        if raw and ms >= next_frame:
            synth.draw(surface)
            raw.write(
                pg.image.tobytes(pg.transform.smoothscale(surface, (W, H)), "RGB")
            )
            next_frame += 1000 / fps
    if a.wav:
        write_wav(a.wav, bytes(audio))
        print(f"wrote {a.wav}")
    if raw:
        raw.close()
        encode(Path(raw.name), bytes(audio), fps, a.video)
        os.unlink(raw.name)
        print(f"wrote {a.video} ({a.ms / 1000:.1f} s)")
    if not (a.video or a.wav or shots):
        synth.draw(surface)
        out = Path(f"{a.shot_prefix or a.app}.png")
        pg.image.save(surface, str(out))
        print(f"wrote {out}")


def play(synth: Synth, a: argparse.Namespace) -> None:
    """The window, with live sound."""
    try:
        import numpy as np
        import sounddevice as sd
    except ModuleNotFoundError as e:
        sys.exit(
            f"error: playing live needs '{e.name}'. Add --with sounddevice --with numpy to uv run."
        )
    pg = synth.pg
    screen = pg.display.set_mode((W, H))
    big = pg.Surface((W * Z, H * Z))
    pg.display.set_caption(
        f"JT-4000: {a.app}    shift + click or key: hold it down    0: let go of all    P: picture    R: video"
    )
    synth.strict = False
    synth.power(False)
    synth.power(True)
    recording: dict | None = None
    taken = 0

    def callback(outdata, frames, _time, _status):
        dll = synth.dll
        if dll is None:
            outdata.fill(0)
        else:
            dll.jt_host_audio(outdata.ctypes.data, frames)
        if recording is not None:
            recording["audio"] += outdata.tobytes()
        np.multiply(outdata, synth.knob["volume"] / 4095, out=outdata, casting="unsafe")

    grabbed: str | int | None = None
    moved = 0
    last_y = 0
    clock = pg.time.Clock()
    with sd.OutputStream(
        samplerate=RATE,
        channels=1,
        dtype="int16",
        blocksize=128,
        latency="low",  # the default adds a tenth of a second between a click and its sound
        callback=callback,
    ):
        while True:
            now = pg.time.get_ticks()
            if a.seconds and now > a.seconds * 1000:
                return
            for e in pg.event.get():
                if e.type == pg.QUIT or (e.type == pg.KEYDOWN and e.key == pg.K_ESCAPE):
                    if recording is not None:
                        recording["raw"].close()
                    return
                if e.type == pg.MOUSEBUTTONDOWN and e.button in (1, 3):
                    what = synth.at(e.pos)
                    if what == "power":
                        synth.power()
                        continue
                    latch = e.button == 3 or pg.key.get_mods() & pg.KMOD_SHIFT
                    if latch and what is not None and what not in synth.knob:
                        synth.latched ^= {
                            what
                        }  # stays down until clicked this way again
                    elif e.button == 1 and what is not None:
                        grabbed, moved, last_y = what, 0, e.pos[1]
                        if isinstance(what, int) or what in BUTTONS:
                            synth.press(what, now)
                elif e.type == pg.MOUSEMOTION and grabbed in synth.pos_knob:
                    dy = last_y - e.pos[1]
                    last_y = e.pos[1]
                    moved += abs(dy)
                    if grabbed == "select":
                        synth.enc += dy
                    else:
                        synth.knob[grabbed] = max(
                            0, min(4095, synth.knob[grabbed] + dy * 14)
                        )
                elif e.type == pg.MOUSEBUTTONUP and e.button == 1:
                    if (
                        grabbed == "select" and moved < 4
                    ):  # a click, not a turn: press it briefly
                        synth.press("select", now)
                        synth.release("select", now)
                    elif isinstance(grabbed, int) or grabbed in BUTTONS:
                        synth.release(grabbed, now)
                    grabbed = None
                elif e.type == pg.MOUSEWHEEL:
                    what = synth.at(pg.mouse.get_pos())
                    if what == "select":
                        synth.enc += e.y * 8
                    elif what in synth.knob:
                        synth.knob[what] = max(
                            0, min(4095, synth.knob[what] + e.y * 120)
                        )
                elif e.type in (pg.KEYDOWN, pg.KEYUP):
                    press = e.type == pg.KEYDOWN
                    ch = pg.key.name(e.key)
                    control: str | int | None = None
                    if len(ch) == 1 and ch in "123456":
                        control = BUTTONS[int(ch) - 1]
                    elif e.key == pg.K_SPACE:
                        control = "select"
                    elif len(ch) == 1 and ch in TYPING:
                        control = WHITE[TYPING.index(ch)]
                    elif ch in TYPING_BLACK:
                        control = TYPING_BLACK[ch]
                    if control is not None:
                        if not press:
                            synth.release(control, now)
                        elif (
                            e.mod & pg.KMOD_SHIFT
                        ):  # with shift: stays down until pressed again
                            synth.latched ^= {control}
                        else:
                            synth.press(control, now)
                    elif press and e.key == pg.K_o:
                        synth.power()  # off, or on again from the start
                    elif press and e.key in (pg.K_0, pg.K_BACKSPACE):
                        synth.latched.clear()  # let go of everything that was left held
                    elif press and e.key in (pg.K_LEFT, pg.K_RIGHT):
                        synth.enc += 8 if e.key == pg.K_RIGHT else -8
                    elif press and e.key == pg.K_p:
                        taken += 1
                        out = Path(f"{a.shot_prefix or a.app}-{taken:02d}.png")
                        pg.image.save(big, str(out))
                        print(f"wrote {out}")
                    elif press and e.key == pg.K_r:
                        if recording is None:
                            recording = {
                                "raw": tempfile.NamedTemporaryFile(  # noqa: SIM115
                                    suffix=".rgb", delete=False
                                ),
                                "audio": bytearray(),
                            }
                            print("recording; press R again to stop")
                        else:
                            done, recording = recording, None
                            done["raw"].close()
                            taken += 1
                            out = Path(f"{a.shot_prefix or a.app}-{taken:02d}.mp4")
                            encode(
                                Path(done["raw"].name), bytes(done["audio"]), 30, out
                            )
                            os.unlink(done["raw"].name)
                            print(f"wrote {out}")
            synth.tick(now)
            synth.draw(big)
            pg.transform.smoothscale(big, (W, H), screen)
            if recording is not None:
                recording["raw"].write(pg.image.tobytes(screen, "RGB"))
                pg.draw.circle(screen, (230, 50, 50), (W - 46, 46), 8)
            pg.display.flip()
            clock.tick(30 if recording is not None else 60)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("app", help="a firmware in sdk/examples, e.g. speak")
    p.add_argument(
        "--ms", type=int, help="no window: run this long, then save a picture and stop"
    )
    p.add_argument(
        "--script",
        default="",
        help="timed inputs for --ms, as in tools/emu.py: '500 tap=lfo; 700 enc+16; 900 key=3; 1000 shot'; also off, on, power",
    )
    p.add_argument(
        "--video",
        type=Path,
        help="with --ms: save a video with sound (.mp4), or a silent .gif",
    )
    p.add_argument("--wav", type=Path, help="with --ms: save the sound")
    p.add_argument(
        "--shot-prefix",
        help="start of the screenshot file names (default: the firmware's name)",
    )
    p.add_argument(
        "--volume", type=int, default=3000, help="VOLUME knob at the start, 0..4095"
    )
    p.add_argument(
        "--seconds",
        type=float,
        help="with a window: close it by itself after this long",
    )
    p.add_argument(
        "--cutoff", type=int, default=4095, help="CUTOFF knob at the start, 0..4095"
    )
    p.add_argument(
        "--resonance", type=int, default=0, help="RESONANCE knob at the start, 0..4095"
    )
    a = p.parse_args()
    lib = build(a.app)
    if a.ms is not None:
        os.environ["SDL_VIDEODRIVER"] = "dummy"
    os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
    try:
        import pygame
    except ModuleNotFoundError:
        sys.exit(
            "error: needs pygame. Run: uv run --with pygame-ce --with sounddevice --with numpy tools/panel.py ..."
        )
    pygame.init()
    synth = Synth(lib, a.app)
    synth.knob["cutoff"], synth.knob["resonance"] = a.cutoff, a.resonance
    synth.knob["volume"] = a.volume
    synth.power(True)
    if a.ms is not None:
        render(synth, a)
    else:
        play(synth, a)
    sys.stdout.flush()
    os._exit(0)  # the firmware's thread never returns


if __name__ == "__main__":
    cli.run(main)
