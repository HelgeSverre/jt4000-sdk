"""Turn a word list into LPC speech data for the speak firmware (sdk/examples/speak).

The way a 1978 Speak & Spell stores its words: each word is rendered once (here by the
`speak` command), cut into 20 ms frames, and each frame is reduced to a loudness, a
voiced/unvoiced flag and ten reflection coefficients of a lattice filter. The pitch is
thrown away, which is why the synth can speak every word at any note.

    uv run --with numpy tools/lpcwords.py                      # writes sdk/examples/speak/words.h
    uv run --with numpy tools/lpcwords.py --preview out.wav    # also decode everything to a WAV
    uv run --with numpy tools/lpcwords.py --voice klatt-female --rate 150
"""

from __future__ import annotations

import argparse
import io
import math
import shutil
import subprocess
import sys
import wave
from pathlib import Path

import cli

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "sdk/examples/speak"
RATE = 8000
FRAME = 160  # 20 ms
WINDOW = 240  # 30 ms analysis window, centred on the frame
ORDER, ORDER_UNVOICED = 10, 6
PRE = 0.9  # pre-emphasis; the firmware undoes it
GAIN_STEPS = 8  # gain codes per factor of two
MAX_FRAMES = 255
BUDGET = 72 * 1024  # bytes of flash left for words after the code


def speak(args: list[str]) -> bytes:
    r = subprocess.run(
        ["speak", "--no-lights", *args], check=False, capture_output=True
    )
    if r.returncode:
        sys.exit(f"error: speak {' '.join(args)}: {r.stderr.decode().strip()}")
    return r.stdout


def render(text: str, voice: str, rate: int):
    """(samples as floats in -1..1, voiced flag per 5 ms) for one word."""
    import numpy as np

    common = ["-v", voice, "-r", str(rate), text]
    raw = speak(
        ["-o", "-", "--file-format=WAVE", f"--data-format=LEI16@{RATE}", *common]
    )
    with wave.open(io.BytesIO(raw)) as w:
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2") / 32768.0
    rows = speak(["--dump", "frames", *common]).decode().splitlines()
    head = rows[0].split("\t")
    t, av = head.index("t_ms"), head.index("av")
    voiced = {
        int(float(r.split("\t")[t])) // 5: float(r.split("\t")[av]) > 0
        for r in rows[1:]
    }
    return pcm, voiced


def levinson(r, order: int):
    """Reflection coefficients and the residual energy left after each order."""
    a = [1.0] + [0.0] * order
    err, ks, errs = r[0], [], []
    for i in range(1, order + 1):
        k = -(r[i] + sum(a[j] * r[i - j] for j in range(1, i))) / err
        k = max(-0.995, min(0.995, k))
        a = [
            a[j] + k * a[i - j] if 0 < j < i else (k if j == i else a[j])
            for j in range(order + 1)
        ]
        err *= 1 - k * k
        ks.append(k)
        errs.append(err)
    return ks, errs


def analyse(pcm, voiced: dict[int, bool]) -> list[tuple[int, bool, list[int]]]:
    """One (gain code, voiced, coefficient codes) per 20 ms frame, silence trimmed."""
    import numpy as np

    x = np.append(pcm[0], pcm[1:] - PRE * pcm[:-1])
    win = np.hamming(WINDOW)
    norm = float(np.sum(win * win))
    pad = (WINDOW - FRAME) // 2
    x = np.concatenate([np.zeros(pad), x, np.zeros(WINDOW)])
    frames = []
    for n in range(len(pcm) // FRAME):
        seg = x[n * FRAME : n * FRAME + WINDOW] * win
        r = [float(np.dot(seg[: WINDOW - i], seg[i:])) for i in range(ORDER + 1)]
        v = voiced.get((n * FRAME + FRAME // 2) * 1000 // RATE // 5, False)
        if r[0] < 1e-9:
            frames.append((0, v, []))
            continue
        r[0] *= 1.0001  # a little white noise keeps the filter away from the edge
        order = ORDER if v else ORDER_UNVOICED
        ks, errs = levinson(r, order)
        rms = math.sqrt(errs[-1] / norm)  # residual level per sample
        gain = round(127 + GAIN_STEPS * math.log2(max(rms, 1e-9)))
        codes = [round(math.asin(k) * 254 / math.pi) for k in ks]
        frames.append((max(0, min(127, gain)), v, codes))
    loud = max(f[0] for f in frames)
    keep = [
        i for i, f in enumerate(frames) if f[0] > loud - 5 * GAIN_STEPS
    ]  # 30 dB down
    frames = frames[max(0, keep[0] - 1) : keep[-1] + 2]
    return frames[:MAX_FRAMES]


def encode(frames) -> bytes:
    """Frame count, then per frame: voiced bit + gain code, then its coefficients
    (ten when voiced, six when not, none for silence)."""
    out = bytearray([len(frames)])
    for gain, v, codes in frames:
        out.append((0x80 if v else 0) | gain)
        if gain:
            out += bytes(c & 0xFF for c in codes)
    return bytes(out)


def decode(blob: bytes, hz: float = 110.0):
    """What the firmware does with one word, at the firmware's settings. For --preview."""
    import numpy as np

    rng = np.random.default_rng(1)
    out, pos = [], 1
    k, g = [0.0] * ORDER, 0.0
    b, phase, carry, y1, was_voiced = [0.0] * (ORDER + 1), 0.0, 0.0, 0.0, None
    for _ in range(blob[0]):
        head = blob[pos]
        v, gain = bool(head & 0x80), head & 0x7F
        n = 0 if not gain else (ORDER if v else ORDER_UNVOICED)
        raw = blob[pos + 1 : pos + 1 + n]
        pos += 1 + n
        tk = [math.sin((c - 256 if c > 127 else c) * math.pi / 254) for c in raw]
        tk += [0.0] * (ORDER - n)
        tg = 2.0 ** ((gain - 127) / GAIN_STEPS) if gain else 0.0
        if not gain:
            tk = k
        if was_voiced is None or v != was_voiced or g == 0.0:
            k = list(tk)  # no gliding between a hiss and a vowel
        was_voiced = v
        dk = [(tk[i] - k[i]) / FRAME for i in range(ORDER)]
        dg = (tg - g) / FRAME
        for _ in range(FRAME):
            g += dg
            k = [k[i] + dk[i] for i in range(ORDER)]
            e, carry = carry, 0.0
            if v:
                phase += hz / RATE
                if phase >= 1.0:
                    phase -= 1.0
                    late = phase / (hz / RATE)  # how long ago the pulse really was
                    amp = math.sqrt(RATE / hz)
                    e += amp * late
                    carry = amp * (1 - late)
            else:
                e = float(rng.uniform(-1.732, 1.732))
            s = g * e
            for i in range(ORDER - 1, -1, -1):
                s -= k[i] * b[i]
                b[i + 1] = b[i] + k[i] * s
            b[0] = s
            y1 = s + PRE * y1
            out.append(y1)
    return np.array(out)


def read_list(path: Path) -> list[tuple[str, list[tuple[str, str]]]]:
    banks: list[tuple[str, list[tuple[str, str]]]] = []
    for n, line in enumerate(path.read_text().splitlines(), 1):
        line = line.split("#")[0].strip()
        if not line:
            continue
        if line.startswith("["):
            banks.append((line.strip("[]").strip().upper(), []))
            continue
        if not banks:
            sys.exit(f"error: {path.name}:{n}: a word before the first [BANK] line")
        name, _, say = (p.strip() for p in line.partition("="))
        name = name.upper()
        if not 1 <= len(name) <= 12 or not name.isascii():
            sys.exit(
                f"error: {path.name}:{n}: '{name}' must be 1 to 12 plain characters"
            )
        banks[-1][1].append(
            (name, say or name.lower() if len(name) > 1 else say or name)
        )
    if not banks or any(not words for _, words in banks):
        sys.exit(f"error: {path.name}: needs at least one [BANK] and no empty banks")
    return banks


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--list", type=Path, default=APP / "words.txt", help="the word list")
    p.add_argument("--out", type=Path, default=APP / "words.h")
    p.add_argument("--voice", default="klatt-male", help="a speak voice (speak -v '?')")
    p.add_argument(
        "--rate", type=int, default=165, help="speaking rate, words per minute"
    )
    p.add_argument("--preview", type=Path, help="decode every word into this WAV file")
    a = p.parse_args()
    if not shutil.which("speak"):
        sys.exit(
            "error: the 'speak' command is not installed. It renders the words:\n"
            "       https://github.com/HelgeSverre/speak (cargo install --path .)\n"
            "       words.h is in the repository, so this is only needed to change the words."
        )
    import numpy as np

    banks = read_list(a.list)
    blobs: list[bytes] = []
    names: list[str] = []
    frames_total = 0
    for _, words in banks:
        for name, say in words:
            frames = analyse(*render(say, a.voice, a.rate))
            frames_total += len(frames)
            blobs.append(encode(frames))
            names.append(name)
    data = b"".join(blobs)
    offsets, at = [], 0
    for blob in blobs:
        offsets.append(at)
        at += len(blob)

    def rows(values, fmt, per):
        return "\n".join(
            "    " + " ".join(fmt.format(v) for v in values[i : i + per])
            for i in range(0, len(values), per)
        )

    sines = [math.sin(i * math.pi / 254) for i in range(128)]
    gains = [0.0] + [2.0 ** ((i - 127) / GAIN_STEPS) for i in range(1, 128)]
    first = 0
    bank_rows = []
    for bank, words in banks:
        bank_rows.append(f'    {{"{bank}", {first}, {len(words)}}},')
        first += len(words)
    text = f"""/* Written by tools/lpcwords.py from {a.list.name}: {len(names)} words, voice {a.voice}.
 * LPC speech: 20 ms frames of a gain, a voiced flag and lattice reflection coefficients. */
#define LPC_RATE {RATE}
#define LPC_FRAME {FRAME}
#define LPC_ORDER {ORDER}
#define LPC_ORDER_UNVOICED {ORDER_UNVOICED}
#define LPC_PRE {PRE}f
#define WORD_COUNT {len(names)}
#define BANK_COUNT {len(banks)}

typedef struct {{ const char *name; uint16_t first, count; }} bank_t;
static const bank_t BANKS[BANK_COUNT] = {{
{chr(10).join(bank_rows)}
}};

static const char WORD_NAMES[WORD_COUNT][13] = {{
{rows(names, '"{}",', 8)}
}};

static const uint16_t WORD_AT[WORD_COUNT] = {{
{rows(offsets, "{},", 12)}
}};

/* coefficient code -> reflection coefficient: sin(code * pi / 254) */
static const float LPC_SIN[128] = {{
{rows(sines, "{:.6f}f,", 6)}
}};

/* gain code -> excitation level */
static const float LPC_GAIN[128] = {{
{rows(gains, "{:.6e}f,", 5)}
}};

static const uint8_t WORD_DATA[{len(data)}] = {{
{rows(list(data), "{},", 24)}
}};
"""
    a.out.write_text(text)
    secs = frames_total * FRAME / RATE
    print(
        f"{len(names)} words in {len(banks)} banks: {len(data)} bytes of speech "
        f"({secs:.0f} s, {len(data) / secs:.0f} bytes per second), "
        f"{len(data) * 100 // BUDGET} % of the {BUDGET // 1024} KiB budget"
    )
    if len(data) > BUDGET:
        sys.exit(f"error: {len(data)} bytes do not fit in {BUDGET}. Remove some words.")
    if max(offsets) > 0xFFFF:
        sys.exit("error: more than 64 KiB of speech needs 32-bit offsets in words.h")
    print(f"wrote {a.out}")
    if a.preview:
        gap = np.zeros(RATE // 8)
        pcm = np.concatenate([np.concatenate([decode(b), gap]) for b in blobs])
        pcm = np.clip(pcm * 32767 * 0.9, -32767, 32767).astype("<i2")
        with wave.open(str(a.preview), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(pcm.tobytes())
        print(f"wrote {a.preview} ({len(pcm) / RATE:.0f} s)")


if __name__ == "__main__":
    cli.run(main)
