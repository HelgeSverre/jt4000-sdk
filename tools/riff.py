"""Turn MIDI files into riffs for the trance firmware (sdk/examples/trance).

Each file becomes one riff: a few bars, snapped to 16th notes, stored relative to its key
so it follows whichever key you hold on the synth. The result is written to
sdk/examples/trance/user_riffs.h, which the firmware picks up on the next build. That
file is git-ignored: riffs made from other people's music stay on your own synth.

    uv run --with mido tools/riff.py song.mid                      # first 4 bars with notes
    uv run --with mido tools/riff.py a.mid b.mid --bars 8
    uv run --with mido tools/riff.py lead.mid --start-bar 16 --tonic A --name LEAD
    uv run --with mido tools/riff.py --clear                       # remove all imported riffs
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import cli

OUT = Path(__file__).resolve().parent.parent / "sdk/examples/trance/user_riffs.h"
NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
# Krumhansl-Kessler key profiles: how strongly each scale degree belongs to a key.
MAJOR = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88]
MINOR = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17]
MAX_USER = 24


def read_notes(
    path: Path, track: int | None
) -> tuple[list[tuple[int, int, int, int]], int]:
    """(start tick, pitch, length in ticks, velocity) for every note, and ticks per beat."""
    import mido

    if not path.is_file():
        sys.exit(f"error: {path}: no such file")
    try:
        mid = mido.MidiFile(path)
    except (OSError, EOFError, ValueError, KeyError, IndexError) as e:
        sys.exit(
            f"error: {path.name}: not a readable MIDI file ({e or type(e).__name__})"
        )
    if track is not None and not 0 <= track < len(mid.tracks):
        sys.exit(
            f"error: {path.name} has tracks 0..{len(mid.tracks) - 1}; --track {track} is not one"
        )
    notes = []
    for ti, tr in enumerate(mid.tracks):
        if track is not None and ti != track:
            continue
        t, on = 0, {}
        for msg in tr:
            t += msg.time
            if msg.type == "note_on" and msg.velocity:
                on[(msg.channel, msg.note)] = (t, msg.velocity)
            elif msg.type in ("note_off", "note_on") and (msg.channel, msg.note) in on:
                start, vel = on.pop((msg.channel, msg.note))
                if getattr(msg, "channel", 0) != 9:  # skip drums
                    notes.append((start, msg.note, t - start, vel))
    return sorted(notes), mid.ticks_per_beat


def guess_tonic(notes: list[tuple[int, int, int, int]]) -> int:
    hist = [0.0] * 12
    for _, pitch, length, _ in notes:
        hist[pitch % 12] += length

    def score(profile: list[float], tonic: int) -> float:
        return sum(hist[(tonic + i) % 12] * profile[i] for i in range(12))

    best = max((score(p, t), t) for t in range(12) for p in (MAJOR, MINOR))
    return best[1]


def convert(
    path: Path, a: argparse.Namespace
) -> tuple[int, list[tuple[int, int, int, int]], str]:
    notes, tpb = read_notes(path, a.track)
    if not notes:
        sys.exit(f"error: {path.name}: no notes found (drum channel 10 is skipped)")
    step_ticks = tpb / 4
    first_bar = (
        int(notes[0][0] / step_ticks) // 16 if a.start_bar is None else a.start_bar
    )
    lo, hi = first_bar * 16, (first_bar + a.bars) * 16
    window = [n for n in notes if lo <= round(n[0] / step_ticks) < hi]
    if not window:
        sys.exit(
            f"error: {path.name}: no notes in bars {first_bar}..{first_bar + a.bars - 1}"
        )
    tonic = NAMES.index(a.tonic.upper()) if a.tonic else guess_tonic(window)
    pitches = sorted(n[1] for n in window)
    median = pitches[len(pitches) // 2]
    ref = median - (median - tonic) % 12  # the tonic at or below the middle of the riff
    loud = sorted(n[3] for n in window)[len(window) * 3 // 4]
    accents = loud > min(n[3] for n in window)  # only when velocities vary
    events, seen = [], set()
    for start, pitch, length, vel in window:
        step = round(start / step_ticks) - lo
        rel = max(-128, min(127, pitch - ref))
        if (step, rel) in seen:
            continue
        seen.add((step, rel))
        ln = max(1, min(255, round(length / (step_ticks / 2))))
        events.append((step, rel, ln, 1 if accents and vel >= loud else 0))
    info = f"{path.name}: bars {first_bar}-{first_bar + a.bars - 1}, key of {NAMES[tonic]}, {len(events)} notes"
    return a.bars * 16, sorted(events), info


def short_name(path: Path, taken: set[str]) -> str:
    base = re.sub(r"[^A-Za-z0-9]", "", path.stem.split("--")[-1]).upper()[:6] or "RIFF"
    name, n = base, 2
    while name in taken:
        name = f"{base[:5]}{n}"
        n += 1
    taken.add(name)
    return name


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("midi", nargs="*", type=Path)
    p.add_argument(
        "--bars", type=int, default=4, help="bars to take (default 4, at most 15)"
    )
    p.add_argument(
        "--start-bar",
        type=int,
        help="first bar, counted from 0 (default: first with notes)",
    )
    p.add_argument("--track", type=int, help="use only this track number")
    p.add_argument(
        "--tonic",
        choices=NAMES + [n.lower() for n in NAMES],
        help="key note (default: guessed)",
    )
    p.add_argument(
        "--name", help="menu name, up to 6 characters (only with a single file)"
    )
    p.add_argument("--clear", action="store_true", help="remove the imported riffs")
    a = p.parse_args()

    if a.clear:
        OUT.unlink(missing_ok=True)
        print(f"removed {OUT}")
        return
    if not a.midi:
        p.error("give at least one MIDI file")
    if not 1 <= a.bars <= 15:
        p.error("--bars must be 1..15")
    if len(a.midi) > MAX_USER:
        p.error(f"at most {MAX_USER} riffs fit in the menu")

    taken: set[str] = set()
    body, table = [], []
    for i, path in enumerate(a.midi):
        steps, events, info = convert(path, a)
        name = (
            a.name.upper()[:6]
            if a.name and len(a.midi) == 1
            else short_name(path, taken)
        )
        print(f"{name:6}  {info}")
        rows = ", ".join("{{{},{},{},{}}}".format(*e) for e in events)
        body.append(f"/* {info} */\nstatic const riff_ev_t RIFF_U{i}[] = {{ {rows} }};")
        table.append(f'    RIFF("{name}", {steps}, RIFF_U{i}),')
    text = (
        "/* Written by tools/riff.py. Not for redistribution: made from third-party MIDI files. */\n"
        + "\n".join(body)
        + "\n#define USER_RIFFS \\\n"
        + " \\\n".join(table)
        + "\n"
    )
    OUT.write_text(text)
    print(f"wrote {OUT} ({len(a.midi)} riffs); rebuild with: make APP=trance flash")


if __name__ == "__main__":
    cli.run(main)
