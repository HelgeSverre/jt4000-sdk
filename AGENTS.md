# Working on JT-4000 firmware

Custom firmware for the Behringer JT-4000 Micro synth (STM32G431, bare-metal C). The user
has the synth; you cannot see or hear it. Check every change in the simulator first, then
ask the user to check it on the synth.

## Build, simulate and flash loop

Run everything from `sdk/`. A firmware is a folder `examples/<name>/` with a `main.c`.

1. Start from a working example: `make new APP=<name>` copies `examples/hello`.
   Read `examples/trance/main.c` before building anything with sound or a menu, and
   `examples/soundbox/main.c` before drawing live pictures.
2. Build with `make APP=<name>`. The build must compile with zero warnings.
3. Simulate: `make APP=<name> sim` draws the display as text. For sound, capture a WAV and
   measure it; never judge audio by reading the code:
   `uv run --with unicorn ../tools/emu.py build/<name>/<name>.bin --ms 3000 --kb 8420 --wav out.wav`
   (`--kb` holds touch keys: `8000` is the lowest key, B2). Required: the display shows
   what you intend, the log has no `CPU FAULT`, and the WAV has the expected pitch and a
   peak below full scale.
4. Flash with `make APP=<name> flash`. It takes three seconds and the synth restarts.
   If it reports "synth not found", ask the user to hold OSC + SETTINGS while switching
   on, then rerun.
5. Ask the user what they see and hear, with specific questions (which knob, which
   number on screen, click or no click). The firmware is done only when the user confirms it.

Report simulator results and hardware results separately: running in the simulator does
not show that a firmware works on the synth.

## Every firmware calls jt_usb_init()

`jt_usb_init()` lets `make flash` restart a running firmware into update mode: the
USB interrupt answers the flasher's request, whatever the main loop is doing. Without it
the firmware is still recoverable, but the user must switch the synth off and on with
OSC + SETTINGS held before every flash.

Flashing is safe to repeat: the factory bootloader validates every image and is never
overwritten. Code that writes flash below `0x08008000` is the only thing that can make the
synth unrecoverable. **The SDK has none; add none.**

## Resource limits

| Resource | Size | Consequence |
|---|---|---|
| RAM | 32 KiB total | A 16-bit delay line at 32 kHz costs 64 KiB per second. Run effects at 16 kHz, and check `bss` in the size line of every build; leave 4 KiB for the stack. |
| Flash | 96 KiB | Enough for code. Tables and samples are what fill it. |
| CPU | 170 MHz, FPU | About 5,000 cycles per sample: float DSP for four voices and effects fits. |
| C library | none | `-nostdlib`. Maths comes from `jt_dsp.h` (`jt_exp2`, `jt_sin`, `jt_note_hz`). There is no `printf`, `malloc` or `sinf`. |
| Display | 128 × 32, 1-bit | 16 × 4 characters of 8 × 8 font. |

The signal path is fixed by the hardware: your samples → DAC → analog filter → output.
Digital effects therefore sit before the filter, and the cutoff knob also filters reverb
tails. The output is mono.

## Audio code

- `render(out, n)` runs in an interrupt, every 2 ms. Do only arithmetic there; do
  display, I2C, delays and MIDI sending in the main loop.
- Share state between the main loop and `render` through plain word-sized variables, and
  wrap multi-field updates (starting a note) in `jt_irq_off()` / `jt_irq_on()`.
- Compute slow things once per block (pitch, filter coefficients, LFO), fast things per
  sample. `examples/soundbox/main.c` shows the split.

## Controls

Copy the input code from `examples/simon/main.c` (shortest) or `examples/groove/main.c`
(modifiers). Each rule below comes from a bug the user found on the synth.

- **Buttons**: a press counts when two readings 8 ms apart agree. Act on the change, not
  the level. One raw reading gave double presses of SELECT.
- **Start-up**: read the inputs a few times before the main loop acts on them. The first
  readings can show every button held.
- **SELECT turn**: one click is 8 counts of `jt_encoder()`; keep the remainder.
- **Touch keys**: poll every 8 to 10 ms; held = two equal readings, released = three empty
  ones. Fingers brush neighbouring keys, so the effect of one wrong touch must be small.
- **Knobs**: a knob takes over a value once it has moved about 90 (of 4095) from where it
  stood when its page opened. Reading it directly makes values jump on a page change, and
  a cutoff knob left at zero makes a firmware that boots silent.
- **Combinations**: hold a button and touch a key, or hold SELECT as shift. Anything
  that erases work takes a held press with a countdown on the display.
- Give each control one job per page and each gesture the same meaning on every page.
  List them in the comment at the top of `main.c` and in the example's README.

## Display

- When the display points at a control, draw the controls as they lie on the synth. Keys
  drawn as a row of boxes were read as the wrong keys; drawn as a piano (ten white keys,
  six black ones on the joins) they were read correctly.
- Show what just happened for long enough to see it: a word while it is spoken, half a
  second of quiet after the user gets something right before the next thing starts.
- For scrolling text, compute the speed from the length of the text and the length of the
  scene; otherwise the scene ends before the text does.
- 17 shades come from a 4 × 4 dither (`SHADE` in `examples/demo/main.c`); `jt_oled_get`
  lets you draw small, read back and redraw large or dissolve between two pictures.

## Sound

- **Timing**: count audio samples in `render` and derive everything from that count:
  sequencer steps, animation time, timeouts. Pictures then stay on the beat however long
  a frame takes, and the firmware runs identically in the simulator and the host build.
- **Level**: sum the voices, multiply by one master gain, `jt_softclip`, then measure the
  WAV: aim for a peak between 0.7 and 0.95. Levels estimated from the code have been
  wrong: one firmware clipped, another played at half level.
- **Speech** (`jt_lpc.h`) is quiet and mid-heavy: lower the band to about 40 % while
  `jt_lpc_busy()` is true to keep it intelligible. A phrase is a queue of words fed from
  the main loop; giving each word its own `hz` makes the voice sing.
- **Echo and reverb** run at 16 kHz (every other sample) to fit the RAM; `examples/demo`
  and `examples/simon` have the eight lines that do it.
- **Resonance**: the analog filter self-oscillates at the top of its resonance range. The
  user wants that; leave the full range reachable.
- **Random**: use a seed that differs per power-on: mix the sample count at the
  first touch with the knobs' raw readings.
- You wrote the presets and factory patterns without hearing them. Say so when you
  report, and ask which ones to change.

## Testing more than the first screen

- **Scripted play**: `tools/panel.py` loads the host build as a library; the same
  four calls (`jt_host_input`, `jt_host_audio`, `jt_host_display`, `jt_host_strict(1)`)
  work from your own Python, faster than real time. A 60-line bot that read the lit
  key off the display and pressed it played `simon` for 13 rounds and found problems a
  screenshot did not show. Import `panel` and use `panel.build(app)` and `panel.boot(lib)`.
- **ARM build**: then run it for a few seconds in `tools/emu.py`. Required: no
  `CPU FAULT`, and the "audio interrupt: worst block" line well under 100 % (the
  examples are at 23 to 30 %).
- **Hardware without the user**: firmware can report over USB. Send a SysEx starting
  `F0 7D` with `jt_midi_send_sysex` every half second and read it with `mido` after
  `make flash`. The storage code was checked on the real EEPROM this way.
- **Pictures**: `just shot <app>` with a `--script` that reaches a representative moment;
  keep the result as `docs/shots/<app>.png`.

## Finishing an example

Done when: zero warnings for ARM and for `make host`; `just lint` clean; a comment at the
top of `main.c` listing every control; `examples/<name>/README.md` with a controls table;
a row in `sdk/README.md`, a line in the main README's quick start, a picture in
`docs/shots/`; and the user has confirmed it on the synth. Commit when asked.

## Look up hardware facts

Take every register address and bit position from the STM32G4 reference manual, RM0440
(download link in `docs/manuals/README.md`; `pdftotext -layout` makes it searchable)
or from the stock firmware's own code in `ghidra/out/v115/decomp.c`, and cite it in a
comment. Other STM32 families use the same register names with different bits; a bit
remembered from another family gave a USB device that never appeared on the bus.

Board wiring (which pin is which knob, I2C addresses, key order) is in `docs/HARDWARE.md`.
When hardware misbehaves, first put the raw values on the display and ask the user to read
them out. Three theories about a "broken filter" were wrong; one screen of numbers showed
the channels were mislabelled.

Give every wait on hardware a bound (`for (i = 0; i < N && !ready; i++)`). An unbounded
wait on the ADC left the synth dark and unresponsive.

## Two ways to run without the synth

- `tools/emu.py` (`make sim`) runs the ARM binary you will flash, about five times slower
  than real time, and prints the display as text. Use it to check the build before flashing.
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
path; the knobs read as fixed values (`--cutoff`, `--resonance`). `--script` presses
buttons, turns SELECT, touches keys and prints the display at set times, e.g.
`--script "500 tap=lfo; 700 enc+16; 900 key=3; 1000 shot"`; use it to check every page of a
firmware. The simulator does not model USB or exact timing: test USB code on the synth,
and a display routine that is fast in the simulator might still be slow on hardware. Read
`uv run --with unicorn ../tools/emu.py --help` for the inputs you can hold.

## Where things are

- `sdk/README.md`: the API in one page. `sdk/include/*.h` are authoritative.
- `docs/BOOTLOADER.md`: read before touching `ld/jt4000.ld`, `src/startup.c` or `tools/fwpack.py`.
  The image format has rules that fail silently (the synth stays in update mode).
- `docs/FIRMWARE-MAP.md`: read to stay compatible with stock presets, CC numbers or SysEx.
- Saving across power-off: `jt_store_*` in `jt4000.h` (see `examples/simon`). Never write the EEPROM
  with `jt_i2c_write` directly: the stock presets and the bootloader's update flag live in it.
- Speech: `jt_lpc.h`; words come from `examples/<name>/words.txt` through `just words <name>`
  (see `examples/simon`). Controls and behaviour of each example: its own `README.md`.
- `docs/HARDWARE.md`: read before adding a driver for anything the SDK does not cover yet.

## Writing docs

- State the fact. Delete a sentence that announces, summarises or judges what follows.
- Use literal verbs and numbers with units: "drops to 40 % of its level", not "steps back".
- A contrast ("a byte sum, not a signature") stays only when the contrast is the finding.
- Headings name their content in sentence case. Bold a control name, a term being defined or a
  safety limit, and nothing else.
- One term per thing: update mode, stock firmware, knobs, button lights, LED/key chip, the user,
  the simulator (`tools/emu.py`), the window (`tools/panel.py`).
- State limits as facts: "Tested on macOS. Not tested on Linux." Use "must" for requirements and
  "might" for uncertain outcomes.
- Keep the anecdote that names the bug behind a rule, and the "verified" / "inference" tags.
- When code makes a statement in a doc false, correct the doc in the same change.

## House rules

- Stock firmware (`firmware/*.bin`) is Behringer's and stays out of git; `uv run tools/fwfetch.py` downloads it.
- `ghidra/out/` (decompiled stock firmware) is generated, not stored: `uv run tools/fwfetch.py --all --decrypt`, then `ghidra/analyze.sh 115` and `ghidra/rename.sh`.
- Python tools run through `uv run`; lint with `uv run ruff check .` and `uv run ruff format .`.
- New SDK features go in `sdk/src` + `sdk/include` with an example that uses them, and a
  row in `sdk/README.md`. Firmware-specific code stays in its `examples/<name>/` folder.
- Comments say why, and where a hardware fact came from.
