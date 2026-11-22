"""Map JT-4000 MIDI CCs to byte offsets in the 64-byte preset record.

For each CC from the Quick Start Guide: send it at several values, dump the active
preset (SysEx 00 00) after each, and report which bytes changed. Only the edit
buffer changes; a program change at the end reloads the stored preset.

    uv run tools/ccmap.py [--channel 1] [--program 1] [-o docs/ccmap.json]
"""

from __future__ import annotations

import argparse
import json
import time

import cli
import mido
from sysex import DUMP_REQUESTS, HEADER, build, collect, open_ports

# Quick Start Guide, "MIDI Continuous Controllers" (QSG_BE_0722-ABR, p. 55)
CCS = {
    1: "Modulation",
    5: "Portamento Time",
    24: "OSC1 Wave",
    25: "OSC2 Wave",
    28: "LFO2 Amount",
    70: "LFO1 Amount",
    71: "VCF Resonance",
    72: "LFO1 Rate",
    73: "LFO2 Rate",
    74: "VCF Cutoff",
    81: "VCA EG Attack",
    82: "VCA EG Decay",
    83: "VCA EG Sustain",
    84: "VCA EG Release",
    85: "VCF EG Attack",
    86: "VCF EG Decay",
    87: "VCF EG Sustain",
    88: "VCF EG Release",
    95: "Ring Modulation Amount",
    96: "Ring Modulation on/off",
    111: "OSC1 Fine Tune",
    112: "OSC2 Fine Tune",
    113: "OSC1 PWM/supersaw Detune/FM Feedback",
    114: "OSC2 PWM",
    115: "OSC1 Coarse Tune",
    116: "OSC2 Coarse Tune",
}
VALUES = (0, 32, 64, 96, 127)


def dump_active(out, inp) -> list[int]:
    for _ in inp.iter_pending():
        pass
    out.send(build(DUMP_REQUESTS["preset"]))
    msgs = collect(inp, 1.5, 0.15)
    # mido strips F0/F7: data = 6 header bytes, cmd 0x15, 64 preset bytes, 00, cs
    for m in msgs:
        if len(m) >= 7 + 64 and tuple(m[:6]) == HEADER and m[6] == 0x15:
            return m[7 : 7 + 64]
    raise RuntimeError(f"no active-preset reply: {msgs!r}")


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--port")
    p.add_argument("--channel", type=int, default=1, help="MIDI channel 1-16")
    p.add_argument("--program", type=int, default=1, help="program to reload at end")
    p.add_argument("-o", "--out")
    args = p.parse_args()
    ch = args.channel - 1

    out, inp, name = open_ports(args.port)
    results = {}
    with out, inp:
        print(f"port: {name}, channel {args.channel}")
        base = dump_active(out, inp)
        for cc, label in CCS.items():
            rows = []
            for v in VALUES:
                out.send(
                    mido.Message("control_change", channel=ch, control=cc, value=v)
                )
                time.sleep(0.05)
                rows.append(dump_active(out, inp))
            changed = sorted(
                {i for r in rows for i in range(64) if r[i] != rows[0][i]}
                | {i for r in rows for i in range(64) if r[i] != base[i]}
            )
            per = {i: [r[i] for r in rows] for i in changed}
            results[cc] = {"name": label, "offsets": per}
            desc = (
                ", ".join(f"[{i}] {per[i]}" for i in changed)
                if changed
                else "no change"
            )
            print(f"CC{cc:>3} {label:38s} {desc}")
        out.send(mido.Message("program_change", channel=ch, program=args.program - 1))
        time.sleep(0.2)
        after = dump_active(out, inp)
        print("restored:", after == base)
    if args.out:
        with open(args.out, "w") as f:
            json.dump(
                {"values_sent": VALUES, "base": base, "ccs": results}, f, indent=1
            )
        print(f"wrote {args.out}")


if __name__ == "__main__":
    cli.run(main)
