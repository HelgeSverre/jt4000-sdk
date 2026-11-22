# groove: a six-track groovebox

The 16 touch keys are the 16 steps of one bar. Six buttons are six tracks. It starts
playing at power-on.

```sh
make APP=groove flash
```

| Track | Button | A step holds | Hold the button and turn CUTOFF | ...and RESONANCE |
|---|---|---|---|---|
| Kick | OSC | soft, normal, accent | Tune | Length |
| Snare | LFO | soft, normal, accent | Body to noise | Length |
| Hat | FILTER | soft, normal, open | Tone | Length |
| Bass | AMP | a note, accent, slide | Filter cutoff | Acid: resonance and envelope |
| Lead | ARP | a note, accent, long | Length | Echo |
| Voice | SETTINGS | a word | Pitch | Size of the mouth |

## Controls

| Control | Does |
|---|---|
| Track button, tap | Show that track's steps on the keys |
| Touch a key | Add a step, or remove the one that is there |
| Hold a step, turn SELECT | Its value: drum level, the note (three octaves of A minor), or the word |
| Hold a step, press SELECT | Bass and lead: accent, slide, both, neither |
| The two knobs | The analog filter over the whole mix: cutoff and resonance |
| Hold a track button, touch a key | One of 16 sounds for that track |
| Hold a track button, turn a knob | Adjust that sound (table above) |
| Hold a track button, turn SELECT | Right: evolve the track, one random change per click. Left: undo, up to 32 changes back |
| Hold a track button, hold SELECT | Clear the track. The display asks "CLEAR KICK?" and counts down one second; letting go cancels |
| SELECT tap | Play / stop |
| SELECT turn | Tempo, 60 to 200 |
| Hold SELECT, touch a key | Go to that pattern at the end of the bar |
| Hold SELECT, press a track button | Mute / unmute the track |
| Hold SELECT, turn | Swing |

Holding SELECT also shows the pattern number, tempo, swing and which tracks are muted.
A knob does nothing after you press or let go of a track button until you move it, so the
sound does not jump.

The display shows all six tracks on top, one row each, and the selected track below as
16 boxes: bar height is the drum level or the note, a dot in the corner is an accent, a
tick at the top right is a slide. The six button lights flash as their tracks play.

Patterns 1 to 6 are filled in (acid house, techno, electro, trance, slow burn, a count-in);
7 to 16 are empty. Everything is forgotten at power-off.

## Sound

Every track has 16 sounds, each a set of six settings; the two knobs adjust the first
two and the other four are fixed by the sound (for example the kick's pitch drop, drive
and click, the bass's wave and envelope, the lead's waves, detune and octave). While a
track button is held the display shows the track, the sound's number and the two knob
settings.

Kick, snare and hat are synthesized, not samples. The bass is a saw through a digital
resonant filter with its own envelope, which is what accent and slide act on; the kick
lowers its level. The voice track is LPC speech (`jt_lpc.h`); the next word cuts off the
one before. The lead, the voice and 8 % of the snare feed a tempo-synced echo. The synth's
analog filter comes last, so the two knobs filter the whole mix.

`words.txt` lists the voice track's words; `tools/lpcwords.py --list ... --out ...` rebuilds `words.h`.
