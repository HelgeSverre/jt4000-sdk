# drone: an additive drone of up to four notes

It sounds all the time: up to four notes, each made of 16 sine partials. The 16 touch keys
are those 16 partials, or 16 of something else, depending on the open page. Six buttons open
six pages, the two knobs shape the open page, and the display draws what you are changing.

```sh
make APP=drone flash
```

| Page (button) | Keys | CUTOFF knob | RESONANCE knob | SELECT turn |
|---|---|---|---|---|
| PARTIALS (OSC) | Partial on / off. Hold keys and turn SELECT: their level | Tilt: dark to bright | Odd / even balance | Transpose |
| MOTION (LFO) | Which partials breathe | Rate | Depth | Transpose |
| FILTER | 16 steps that pulse the analog filter | Cutoff | Resonance | Pulse speed |
| WARP (AMP) | Which partials beat | Stretch: squeezed, harmonic, bell | Fold | Transpose |
| CHORD (ARP) | The notes, up to four | Spread | Drift | Transpose |
| SPACE (SETTINGS) | 16 scenes: tap to morph to one, hold a second to store | Reverb | Echo | Morph time |

SELECT press silences it and brings it back. A USB MIDI note sets the root.

A knob does nothing after a page change until you move it, so opening a page does not
change the sound. A scene holds the partials, notes, steps and all twelve knob settings.
Stored scenes are forgotten at power-off.

## How the sound is made

Each partial of each note is its own sine oscillator (64 at most), so partials can be
moved off the harmonic series (stretch), detuned against each other (beat, spread) and
faded on their own slow clocks (motion), none of which a single wavetable can do. The
loudness is kept level as partials come and go. After the oscillators: a wavefolder, echo
and reverb at 16 kHz, then the synth's analog filter, which the FILTER page pulses.
