# choir: frozen speech as a pad

LPC speech stores a word as a row of 20 ms snapshots of the mouth's shape. A speech
synthesizer runs through them in order. This one stops wherever you like: one knob moves
through the word, and up to four voices sing the sound found there, at the notes you hold,
for as long as you hold them. At power-on it holds a chord and drifts through "ALLELUIA".

```sh
make APP=choir flash
```

| Control | Does |
|---|---|
| Keys | Sing, up to four notes |
| SELECT turn | The word (34 of them, starting with the plain vowels AH, EE, OH, OO) |
| SELECT press | Hold on / off. On: notes keep sounding after you let go, and a new chord replaces the old one. Off: silence. On again: the last chord comes back |
| USB MIDI notes | Sing too |

Six buttons open six pages. The two knobs belong to the open page and do nothing after a
page change until moved.

| Page (button) | CUTOFF knob | RESONANCE knob |
|---|---|---|
| WORD (OSC) | Position in the word | Size of the mouth: giant to child |
| MOTION (LFO) | Drift: the position wanders | Travel: left backwards, middle still, right forwards; further is faster |
| FILTER | Cutoff | Resonance (the analog filter) |
| SHAPE (AMP) | Attack | Release |
| CHOIR (ARP) | Spread: two singers per note, detuned | Vibrato |
| SPACE (SETTINGS) | Reverb | Breath: sung to whispered |

## The display

Top: the word and the page. Bottom: the word as a line of loudness, solid where it is
sung and dotted where it is hissed (s, k, t), with a cursor where you are.

Middle, on the WORD and MOTION pages: the mouth and throat as a tube. This is not a
decoration. LPC's ten numbers per snapshot are reflection coefficients, each the step in
width between two neighbouring sections of a tube, which is how the method models a vocal
tract. The picture is those ten steps drawn as widths.

The other pages draw the filter curve, the envelope, four mouths that open as each voice
sounds, and the output wave.

## Changing the words

`words.txt` lists them. A slow speaking rate gives more snapshots per word and a smoother
ride through it:

```sh
uv run --with numpy tools/lpcwords.py --list sdk/examples/choir/words.txt --out sdk/examples/choir/words.h --rate 110
```
