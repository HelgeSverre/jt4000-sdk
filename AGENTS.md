# Working on JT-4000 firmware

Custom firmware for the Behringer JT-4000 Micro synth (STM32G431, bare-metal C). A real
person is holding the synth; you cannot see or hear it. Your eyes are the **simulator**,
theirs are the hardware. Use both, in that order.

## The loop

Run everything from `sdk/`. A firmware is a folder `examples/<name>/` with a `main.c`.

1. **Start** from a working example: `make new APP=<name>` copies `examples/hello`.
   Read `examples/trance/main.c` before building anything with sound or a menu, and
   `examples/soundbox/main.c` for drawing live pictures.
2. **Build**: `make APP=<name>`. Done when it compiles with zero warnings.
3. **Simulate**: `make APP=<name> sim` draws the display as text. For sound, capture and
   measure it; never judge audio by reading the code:
   `uv run --with unicorn ../tools/emu.py build/<name>/<name>.bin --ms 3000 --kb 8420 --wav out.wav`
   (`--kb` holds touch keys: `8000` is the lowest key, B2). Done when the display shows
   what you intend, the log has no `CPU FAULT`, and the WAV has the expected pitch and a
   peak below full scale.
4. **Flash**: `make APP=<name> flash`. Three seconds, and the synth restarts itself.
   If it reports "synth not found", ask the user to hold OSC + SETTINGS while switching
   on, then rerun.
5. **Ask the user** what they see and hear, with specific questions (which knob, which
   number on screen, click or no click). The firmware is done when they confirm it, not
   when it flashes.

Report simulator results and hardware results separately. "Runs in the simulator" and
"works" are different claims.

## Every firmware keeps the way home

Call `jt_usb_init()` once and `jt_poll_escape()` in the main loop, and keep that loop
turning. Those two calls are what let `make flash` and OSC + SETTINGS reach update mode
from running firmware. Without them the user must power-cycle with the buttons held:
recoverable, but it costs them a trip to the synth each time.

Flashing is safe to do freely: the factory bootloader validates every image and is never
overwritten. Flash code that writes below `0x08008000` is the one thing that could brick
the synth; the SDK has none, so add none.

## The budget

| Resource | Size | Consequence |
|---|---|---|
| RAM | 32 KiB total | A 16-bit delay line at 32 kHz costs 64 KiB per second. Run effects at 16 kHz, and check `bss` in the size line of every build; leave 4 KiB for the stack. |
| Flash | 96 KiB | Plenty for code; tables and samples add up fast. |
| CPU | 170 MHz, FPU | About 5,000 cycles per sample: float DSP for four voices and effects fits. |
| C library | none | `-nostdlib`. Maths comes from `jt_dsp.h` (`jt_exp2`, `jt_sin`, `jt_note_hz`). No `printf`, `malloc`, `sinf`. |
| Display | 128 × 32, 1-bit | 16 × 4 characters of 8 × 8 font. |

The signal path is fixed by the hardware: your samples → DAC → **analog filter** → output.
Digital effects therefore sit before the filter, and the cutoff knob darkens reverb tails
too. Mono only.

## Audio code

- `render(out, n)` runs in an interrupt, every 2 ms. Only arithmetic belongs there:
  display, I2C, delays and MIDI sending belong in the main loop.
- Share state between the main loop and `render` through plain word-sized variables, and
  wrap multi-field updates (starting a note) in `jt_irq_off()` / `jt_irq_on()`.
- Compute slow things once per block (pitch, filter coefficients, LFO), fast things per
  sample. `examples/soundbox/main.c` shows the split.

## Controls: what the examples learned

Copy the input code from `examples/simon/main.c` (shortest) or `examples/groove/main.c`
(modifiers); each rule below is a bug a person found on the synth.

- **Buttons**: a press counts when two readings 8 ms apart agree, and you act on the
  change, not the level. One raw reading gave double presses of SELECT.
- **Start-up**: read the inputs a few times before the main loop acts on them. The first
  readings can show every button held.
- **SELECT turn**: one click is 8 counts of `jt_encoder()`; keep the remainder.
- **Touch keys**: poll every 8 to 10 ms; held = two equal readings, released = three empty
  ones. Fingers brush neighbouring keys, so a wrong touch should cost little.
- **Knobs**: a knob takes over a value once it has moved about 90 (of 4095) from where it
  stood when its page opened. Reading it directly makes values jump on a page change, and
  a cutoff knob left at zero makes a firmware that boots silent.
- **Two things at once**: hold a button and touch a key, or hold SELECT as shift. Anything
  that erases work takes a held press with a countdown on the display.
- **One job per control per page**, the same gesture meaning the same thing on every page,
  listed in the comment at the top of `main.c` and in the example's README.

## The display: what the examples learned

- When the display points at a control, draw the controls as they lie on the synth. Keys
  drawn as a row of boxes were read as the wrong keys; drawn as a piano (ten white keys,
  six black ones on the joins) they were found at once.
- Show what just happened for long enough to see it: a word while it is spoken, half a
  second of quiet after the player gets something right before the next thing starts.
- Text that scrolls is given the time it needs: compute the speed from the length of the
  text and the length of the scene, or the end is never seen.
- 17 shades come from a 4 × 4 dither (`SHADE` in `examples/demo/main.c`); `jt_oled_get`
  lets you draw small, read back and redraw large or dissolve between two pictures.

## Sound: what the examples learned

- **One clock.** Count audio samples in `render` and derive everything from that count:
  sequencer steps, animation time, timeouts. Pictures then stay on the beat however long
  a frame takes, and the firmware runs identically in both simulators.
- **Level.** Sum the voices, multiply by one master gain, `jt_softclip`, then measure the
  WAV: aim for a peak between 0.7 and 0.95. Loudness judged from the code was wrong more than once (clipping, and a firmware at half level).
- **Speech** (`jt_lpc.h`) is quiet and mid-heavy: drop the band to about 40 % while
  `jt_lpc_busy()` and it becomes intelligible. A phrase is a queue of words fed from the
  main loop; giving each word its own `hz` makes the robot sing.
- **Echo and reverb** run at 16 kHz (every other sample) to fit the RAM; `examples/demo`
  and `examples/simon` have the eight lines that do it.
- **The analog filter's resonance sings on its own at the top of its range.** The owner
  likes that; leave the full range reachable.
- **Random** things need a seed that differs per power-on: mix the sample count at the
  first touch with the knobs' raw readings.
- Presets and factory patterns you wrote without hearing them are guesses. Say so when
  you report, and ask which ones to change.

## Testing more than the first screen

- **Play it with a script.** `tools/panel.py` loads the host build as a library; the same
  four calls (`jt_host_input`, `jt_host_audio`, `jt_host_display`, `jt_host_strict(1)`)
  work from your own Python, as fast as the computer goes. A 60-line bot that read the lit
  key off the display and pressed it played `simon` for 13 rounds and found what a
  screenshot could not. Import `panel` and use `panel.build(app)` and `panel.boot(lib)`.
- **Then the ARM build**, a few seconds in `tools/emu.py`: no `CPU FAULT`, and the "audio
  interrupt: worst block" line well under 100 % (the examples sit at 23 to 30 %).
- **Hardware without a person.** Firmware can report over USB: send a SysEx starting
  `F0 7D` with `jt_midi_send_sysex` every half second and read it with `mido` after
  `make flash`. The storage code was checked on the real EEPROM this way.
- **Pictures**: `just shot <app>` with a `--script` that reaches a telling moment; keep the
  result as `docs/shots/<app>.png`.

## Finishing an example

Done when: zero warnings for ARM and for `make host`; `just lint` clean; a comment at the
top of `main.c` listing every control; `examples/<name>/README.md` with a controls table;
a row in `sdk/README.md`, a line in the main README's quick start, a picture in
`docs/shots/`; and the person has confirmed it on the synth. Commit when asked.

## Hardware facts are looked up

Every register address and bit position comes from the STM32G4 reference manual, RM0440
(download link in `docs/manuals/README.md`; `pdftotext -layout` makes it searchable)
or from the stock firmware's own code in `ghidra/out/v115/decomp.c`, cited in a comment.
Other STM32 families use the same register names with different bits; a remembered bit
from the wrong family cost this project a dead USB port.

Board wiring (which pin is which knob, I2C addresses, key order) is in `docs/HARDWARE.md`.
When hardware misbehaves, first put the raw values on the display and ask the user to read
them out. Three theories about a "broken filter" were wrong; one screen of numbers showed
the channels were mislabelled.

Every wait on hardware has a bound (`for (i = 0; i < N && !ready; i++)`). An unbounded
wait turns a small bug into a synth that looks dead.

## Two ways to run without the synth

- `tools/emu.py` (`make sim`) runs the ARM binary you will flash, about five times slower
  than real time, and prints the display as text. Use it to prove a build works.
- `tools/panel.py <app>` compiles the same source for the computer (`make host`, with
  `sdk/host/host.c` in place of the drivers) and shows it in a window with sound, or
  renders a picture or video: `--ms 5000 --script "..." --video out.mp4`. Use it to hear a
  change quickly and to make pictures for a README. Its analog filter is an approximation,
  it has no USB, and code that only breaks on ARM will not break there.

Both take the same `--script`. Firmware must build for both: no ARM assembly in an
example (use `jt_sqrt`, `jt_irq_off`, `jt_irq_on`), and a main loop that calls
`jt_delay_ms` or `jt_oled_flush` every turn.

## What the simulator does and does not show

`tools/emu.py` models the CPU, display, lights, buttons, touch keyboard and the audio
path; the knobs read as fixed values (`--cutoff`, `--resonance`). `--script` presses buttons, turns SELECT, touches keys and prints the display at set times, e.g. `--script "500 tap=lfo; 700 enc+16; 900 key=3; 1000 shot"`; use it to check every page of a firmware. USB and exact timing are outside it: USB code is tested on the synth only,
and a display routine that is fast in the simulator may still be slow on hardware. Read
`uv run --with unicorn ../tools/emu.py --help` for the inputs you can hold.

## Where things are

- `sdk/README.md`: the API in one page. `sdk/include/*.h`: the authority.
- `docs/BOOTLOADER.md`: read before touching `ld/jt4000.ld`, `src/startup.c` or `tools/fwpack.py`.
  The image format has rules that fail silently (the synth stays in update mode).
- `docs/FIRMWARE-MAP.md`: read to stay compatible with stock presets, CC numbers or SysEx.
- Saving across power-off: `jt_store_*` in `jt4000.h` (see `examples/simon`). Never write the EEPROM
  with `jt_i2c_write` directly: the stock presets and the bootloader's update flag live in it.
- Speech: `jt_lpc.h`; words come from `examples/<name>/words.txt` through `just words <name>`
  (see `examples/simon`). Controls and behaviour of each example: its own `README.md`.
- `docs/HARDWARE.md`: read before adding a driver for anything the SDK does not cover yet.

## House rules

- Factory firmware (`firmware/*.bin`) is Behringer's and stays out of git; `uv run tools/fwfetch.py` downloads it.
- `ghidra/out/` (decompiled stock firmware) is generated, not stored: `uv run tools/fwfetch.py --all --decrypt`, then `ghidra/analyze.sh 115` and `ghidra/rename.sh`.
- Python tools run through `uv run`; lint with `uv run ruff check .` and `uv run ruff format .`.
- New SDK features go in `sdk/src` + `sdk/include` with an example that uses them, and a
  row in `sdk/README.md`. Firmware-specific code stays in its `examples/<name>/` folder.
- Comments say why, and where a hardware fact came from.
