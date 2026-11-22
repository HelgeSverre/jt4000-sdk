# acid: a bassline machine

One bass line in the manner of the TB-303, with a kick and a hat to play against. 16 steps
on the 16 keys. It starts playing at power-on.

```sh
make APP=acid flash
```

## The six buttons choose what the keys edit

| Button | The keys | Hold a step and turn SELECT |
|---|---|---|
| NOTE (OSC) | A note here or not | Its pitch, C to C |
| ACCENT (LFO) | Accent on / off | |
| SLIDE (FILTER) | Slide into the next note on / off | |
| OCTAVE (AMP) | Normal, up, down | |
| KICK (ARP) | A kick here or not | Soft, normal, accent |
| HAT (SETTINGS) | A hat here or not | Soft, normal, open |

## The knobs

With nothing held they are the bass filter's CUTOFF and RESONANCE. Hold a button and they
become that button's pair; the display names them and shows their values.

| Hold | CUTOFF knob | RESONANCE knob |
|---|---|---|
| OSC | Tune | Wave: saw or square |
| LFO | Env mod | Decay |
| FILTER | Accent | Slide time |
| AMP | Drive | Echo |
| ARP | Kick tune | Kick length |
| SETTINGS | Hat tone | Hat length |

A knob does nothing after a button is pressed or let go until you move it.

## Everything else

| Control | Does |
|---|---|
| Hold OSC, touch a key | One of 16 bass sounds (all ten bass settings at once) |
| Hold a button, turn SELECT | Right: change that part of the line at random, one change per click. Left: undo, up to 32 back |
| Hold a button, hold SELECT | Clear that part of the line. The display counts down a second; letting go cancels |
| SELECT tap | Play / stop |
| SELECT turn | Tempo, 60 to 200 |
| Hold SELECT, touch a key | Go to that pattern at the end of the bar |
| Hold SELECT, turn | Swing |
| Hold SELECT, press FILTER | Digital filter / analog filter |
| Hold SELECT, press ARP or SETTINGS | Mute the kick or the hat |

Holding SELECT shows the pattern, tempo, swing and which filter is in use. Patterns 1 to 8
are filled in; 9 to 16 are empty. Everything is forgotten at power-off.

The display: accents are blocks along the top, slides are lines under them reaching into
the next step, notes are bars whose height is the pitch, kick and hat are the two rows
below, and the line at the bottom is the step being played. The open layer's button is
lit; LFO, ARP and SETTINGS flash on accents, kicks and hats.

## What makes it acid

- **Accent** makes a note louder, opens the filter further, and gives it a short fixed
  decay. Accents in a row pile up, each opening the filter more than the last.
- **Slide** holds the note through to the next one and glides the pitch there, with no
  new attack and no restart of the envelope.
- **The filter** is a four-pole ladder with the resonance fed back around it and a soft
  limit in the loop, so it screams before it whistles. The envelope moves it sample by
  sample.
- **Analog filter mode** bypasses the ladder and drives the synth's own analog filter with
  the same envelope, 500 times a second. It is the real thing, but the kick and hat go
  through it too (the synth has one filter, at the output), and how fast it follows the
  envelope depends on the hardware.
