# speak: a talking typewriter

Pick words with the SELECT knob, touch a key to add each one at a note, and play the
sentence back. At power-on it says "hello, I am a synthesizer, running custom firmware".

```sh
make APP=speak flash
```

## Controls

The display has two rows, the word to add (top) and the sentence (bottom); pressing SELECT
moves the knob from one row to the other.

| Control | Word row | Sentence row |
|---|---|---|
| SELECT turn | Choose a word | Move the cursor from word to word |
| Touch a key | Add the word at that note, after the cursor | Give the word under the cursor that note |
| SELECT press | Go to the sentence row | Go to the word row |

Either way the word is spoken when you stop turning. In the word row a blinking caret
shows where the next word will go; in the sentence row the word under the cursor is
highlighted and shown large. The cursor stays where you leave it, so to put a word in the
middle: go to the sentence row, stop on the word before the gap, go back, add.

| Control | Does |
|---|---|
| OSC | Next word bank: letters, numbers, words. Hold it and turn SELECT to jump by first letter |
| LFO | Voice settings (below) |
| FILTER | Add a pause after the cursor |
| AMP | Delete the word at the cursor, like backspace. Hold to clear the sentence |
| ARP | Play / stop, always from the start |
| SETTINGS | Loop on / off |
| CUTOFF knob | Speed |
| RESONANCE knob | Size of the mouth, giant to child |
| USB MIDI notes | Each note speaks the next word of the sentence at that note |

While a sentence plays, touching a key re-pitches the word being spoken. A bar above each
word in the sentence shows the height of its note. A sentence holds 32 words and is
forgotten at power-off.

## Voice settings

Each press of LFO shows the next setting; SELECT turn changes it. On this page the keys
speak the word without adding it, so you can hear the change. SELECT press goes back.

| Setting | Range | Does |
|---|---|---|
| OCTAVE | -2..2 | Whole octaves down or up |
| TONE | 0..10 | 0 is one bare click per period, the Speak & Spell buzz; 10 is round and dull |
| BREATH | 0..10 | Noise mixed into the voice; 10 is a whisper |
| ROUGH | 0..10 | Uneven periods: each one varies in length at random |
| VIBRATO | 0..10 | Depth, up to 0.7 semitones |
| BEND | -10..10 | The pitch falls (-) or rises (+) through each word, up to 7 semitones |
| DOUBLE | 0..10 | A second voice, detuned by up to 4 % |

## How it speaks

The method of the 1978 Speak & Spell, linear predictive coding. Each word is rendered once
on the computer, cut into 20 ms frames, and every frame reduced to a loudness, a
voiced/unvoiced flag and ten coefficients of a lattice filter: about 500 bytes per second
of speech. The synth feeds that filter with pulses (voiced) or noise (unvoiced). The pitch
of the original is not stored, which is why any word can be said at any note.

Running text-to-speech on the synth itself does not fit: the letter-to-sound rules alone
are 1.5 MB, the synth has 96 KiB.

## Your own words

`words.txt` is the word bank, one word per line, in banks. `words.h` is made from it:

```sh
uv run --with numpy tools/lpcwords.py                       # from the repository root
uv run --with numpy tools/lpcwords.py --voice klatt-female --preview all.wav
```

This needs the [`speak`](https://github.com/HelgeSverre/speak) command, which renders the
words. The tool prints how much of the flash budget the bank uses; 267 words take 55 KiB.
Names of up to 8 characters are drawn large, up to 12 fit.
