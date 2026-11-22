# demo: a minute and a half of music and pictures, then a playable synth

Music, pictures and a few spoken words, in the tradition of the demoscene: nothing is
stored that can be computed. When it ends, the keys play a four-voice synth.

```sh
make APP=demo flash                  # on the synth
just video demo 110000 demo.mp4      # or as a video, without the synth
```

## Controls while it plays

| Control | Does |
|---|---|
| Keys | Shift the whole song to another key. The A key is home; it stays where you leave it |
| CUTOFF, RESONANCE | The analog filter, from the moment you move them |
| SELECT turn | How the picture is shown: plain, negative, scan lines, mesh, mirrored, upside down |
| SELECT press | Negative on / off |
| LFO, FILTER, AMP, ARP (held) | No drums / no bass / no arpeggio and lead / no pad |
| OSC | Skip to the end |

Afterwards the keys play, four voices; the knobs are the filter; SELECT press starts the
demo again.

## Scenes by bar

| Bars | Picture | Music |
|---|---|---|
| 1-4 | The name arrives out of a starfield; "hello, human" | Pad, arpeggio and 16th-note hats; a roll in bar 4 |
| 5-12 | A solid cube lit from the front, facts scrolling underneath | Kick, bass, hats |
| 13-20 | Plasma that brightens on each kick | Clap and lead join |
| 21-24 | The break: "this is custom firmware" over a fire that grows | Pad only; the analog filter closes and opens |
| 25-28 | A turning chequerboard with the sound drawn across it | Everything |
| 29-32 | A ship shoots the words BOOTLOADER and SYNTH to bits, letter by letter | Everything; the voice holds one "wee", its pitch changed on every 16th |
| 33-36 | Four drums, and a ball dropping onto each exactly as it sounds | A drum solo: kick, snare, hat, tom |
| 37-44 | The repository's address on a wave, once every two bars; "free and open source, get the code" | Everything, arpeggio an octave up; bar 44 is pad, arpeggio and a roll only |
| 45-48 | Down a tube | The whole song two semitones higher |
| 49-52 | A twisted bar | The same |
| 53-56 | "Now you, touch the keys" | No drums; the filter closes |

## How it is made

- The song and the pictures share one clock. The song counts audio samples: a 16th note is
  exactly 3750 of them (128 beats a minute). The pictures read that count, so a flash lands
  on its kick however long a frame takes to draw, and a scroller's last letter leaves as
  its scene ends.
- The song is `parts()`, which says what plays in each bar, a four-chord round (A minor,
  F, C, G), a 16-step bass figure, an arpeggio that falls from the top of the chord and
  leaves gaps, a 64-step melody and a list of when to say which word. Every sound is
  synthesized; the voice is LPC speech (`jt_lpc.h`), and while it speaks the other parts
  drop to 40 % of their level (80 % in bars 29-32).
- The pictures are arithmetic on a 256-entry sine table. Shades come from a 4 x 4 dither
  pattern, 17 levels on a display that has two. The cube's faces are filled polygons lit
  by how far they face the viewer; the tube uses two small tables, worked out at start-up,
  that say for each patch of screen which way round the tube it looks and how far down.
- The shooter's bullets and debris, and the balls over the drums, keep no state: each frame
  works out where everything is from the time alone. The balls read the same drum pattern
  the sequencer plays, which is why they land on the beat.
