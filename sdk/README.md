# JT-4000 Micro SDK

Build custom firmware for the Behringer JT-4000 Micro in C and flash it over USB, without
SynthTribe or a debug probe.

Everything here has run on a real JT-4000 at the full 170 MHz clock.

## Requirements

- `arm-none-eabi-gcc` and `arm-none-eabi-binutils` (Homebrew formulas of the same names)
- `uv` (runs the packaging, flashing and simulator tools in `../tools`)

## Use

These are the `make` targets, run inside `sdk/`. The repository's `justfile` wraps them for use
from the root (`just flash acid`); see the main README.

```
make                    # build examples/hello -> build/hello/hello.cry.bin
make flash              # build and flash (about 3 seconds)
make sim                # run in the simulator and draw the display
make new APP=myapp      # start examples/myapp from examples/hello
make APP=myapp flash
uv run --with unicorn ../tools/emu.py build/tone/tone.bin --key --wav out.wav   # capture the DAC
```

Flashing needs the synth in update mode. `make flash` restarts the synth into update mode
when it runs stock firmware, or SDK firmware that calls `jt_usb_init()`. Otherwise hold
**OSC + SETTINGS** while switching the synth on.

To return to stock firmware: `uv run ../tools/flash.py flash ../firmware/JT-4000_INTFLASH_CRY_V115.bin`.

## API (`include/jt4000.h`)

| Function | Does |
|---|---|
| `jt_init()` | 170 MHz clock, GPIO, display, lights, knobs, keyboard |
| `jt_delay_ms(ms)` | blocking delay (SysTick, polled) |
| `jt_oled_clear/pixel/text/flush` | 128×32 frame buffer; 8×8 font gives 16×4 characters |
| `jt_oled_line`, `jt_oled_text_big`, `jt_oled_get`, `jt_oled_invert` | lines, scaled text, pixel read-back, whole-panel invert |
| `jt_button_osc()`, `jt_button_settings()` | the two buttons wired to the MCU |
| `jt_leds(mask)` | the six button lights (`JT_LED_*`) |
| `jt_keys_raw(buf)` | scanned buttons: LFO, FILTER, AMP, ARP, SELECT press (`JT_KEY_*`) |
| `jt_pot(n)` | `JT_ADC_CUTOFF`, `JT_ADC_RESONANCE`, `JT_ADC_KEYBOARD` (key-down line), 0..4095 |
| `jt_keyboard()` | 16 key bits; `jt_keyboard_lowest()` gives the lowest held key |
| `jt_encoder()` | SELECT knob position |
| `jt_i2c_write/read` | the bus with the EEPROM and LED/key chip |
| `jt_store_read/write(at, data, n)` | 24 KiB that survives power-off: the part of the EEPROM the stock firmware never uses, so its presets stay intact. Main loop only; 0.4 ms a byte to read, about 50 ms per 64 changed bytes to write |
| `jt_store_save/load(at, tag, data, n)` | the same with a name and a checksum in front (n + 8 bytes); `load` leaves `data` alone unless an intact record with that tag is there, so defaults stay |
| `jt_audio_init(render)` | 12-bit DAC at 32 kHz; `render(out, n)` is called from an interrupt for each 2 ms block |
| `jt_filter(cutoff, resonance)` | the analog filter, each 0..1000 |
| `jt_usb_init()` | class-compliant USB-MIDI device "JT-4000 SDK" (`1397:127D`), runs from the USB interrupt. Also answers the flasher's "reboot to update mode" message from that interrupt, so `make flash` works whatever the main loop is doing |
| `jt_midi_get(ev)` | next received USB-MIDI event (4 bytes), false when none |
| `jt_midi_send(status, d1, d2)`, `jt_midi_send_sysex(msg, n)` | send to the host |
| `jt_poll_escape()` | optional: call in the main loop to make OSC + SETTINGS, held while the firmware runs, restart into update mode |
| `jt_enter_update_mode()` | set the bootloader's EEPROM flag and reset |
| `jt_reset()` | system reset |
| `jt_irq_off()`, `jt_irq_on()` | hold the audio interrupt off around a change it must see whole (starting a note) |

## Sound blocks (`include/jt_dsp.h`)

Each block is one struct and one `next` function, works in float and uses no C library.

| Block | Purpose |
|---|---|
| `jt_osc_t` | saw, square (both band-limited), triangle, sine, noise |
| `jt_env_t` | attack, decay, sustain, release |
| `jt_lfo_t` | sine, triangle, square, random steps |
| `jt_svf_t` | digital low/band/high-pass filter, stable while swept |
| `jt_tape_t` | a loop of recent audio in your own buffer: delays, and the source for grains |
| `jt_grains_t` | four overlapping slices of the tape replayed at another pitch |
| `jt_reverb_t` | four cross-fed delay lines, 5 KiB, tuned for a 16 kHz call rate |
| `jt_exp2`, `jt_note_hz`, `jt_sin`, `jt_rand`, `jt_softclip`, `jt_sqrt` | maths helpers |

The DAC feeds the analog filter, so digital effects sit before it: closing the cutoff knob also closes the reverb tail.

## Speech (`include/jt_lpc.h`)

LPC speech, the method of the Speak & Spell: about 500 bytes per second of speech, and the
pitch is not part of a word, so any word can be said or sung at any note.

1. List the words in `examples/<app>/words.txt` (one per line, `NAME = what to say` when they differ).
2. `just words <app>` (or `uv run --with numpy ../tools/lpcwords.py --list ... --out ...`) writes `words.h`. Needs the [`speak`](https://github.com/HelgeSverre/speak) command.
3. In the firmware: `jt_lpc_init(&v, LPC_SIN, LPC_GAIN)`, `jt_lpc_say(&v, &WORD_DATA[WORD_AT[n]])` from the main loop, `jt_lpc_block(&v)` once per block and `jt_lpc_next(&v)` per sample in `render`. Set `v.hz`, `v.ratio` (size of the mouth) and `v.speed` at any time.

`jt_lpc_frame` and `jt_lattice_next` give the frames and the vocal-tract filter on their own (`examples/choir`). `examples/simon` and `examples/demo` are the shortest examples that use the player.

## Menu (`include/jt_menu.h`)

Describe up to six pages (one per panel button) as tables of items that point at your `int16_t` variables; call `jt_menu_poll()` every 10 ms. Turn SELECT to choose, press to edit. See `examples/trance`.

## On the computer

`make host APP=name` builds a firmware as a shared library for this computer, with
`host/host.c` standing in for the hardware; `uv run --with pygame-ce --with sounddevice
--with numpy ../tools/panel.py name` shows it in a window that looks like the synth and
plays it. See the main README.

## Examples

| Example | Shows |
|---|---|
| `hello`, `panel`, `bringup` | display, inputs, start-up stages |
| `tone` | one oscillator, keyboard, USB MIDI notes and CCs |
| `soundbox` | An ambient sound box with no text: a generator plays notes from a scale, six buttons open six animated scenes and the two knobs shape the open one. Shows echo, grains, reverb and a boot animation. |
| `trance` | A seven-saw supersaw with a trance gate, tempo-synced delay, presets, and built-in riffs and an arpeggiator that follow the held key. `tools/riff.py` imports MIDI files as extra riffs. |
| `demo` | A demoscene-style demo: an eight-part tracker with a speaking voice plays 56 bars, with pictures such as a starfield, a shaded cube and plasma timed from the audio sample count. Keys, knobs and SELECT change it while it plays; it ends as a four-voice synth. |
| `acid` | A bassline machine in the manner of the TB-303: 16 steps with accent, slide and octave, through a four-pole ladder filter or the synth's analog filter driven by the same envelope. Controls in `examples/acid/README.md`. |
| `simon` | Simon with melodies: the robot plays a tune that grows by a note a round, you play it back, and it taunts you in LPC speech when you miss. Three games (classic, by ear, backwards); best scores are kept with `jt_store_save`. |
| `choir` | Frozen speech as a pad: a knob moves through an LPC word and stops anywhere, and up to four voices sing the mouth shape found there. Uses the frame-level API of `jt_lpc.h` and draws the vocal tract on the display. |
| `drone` | An additive drone that always sounds: the 16 keys edit 16 sine partials per note, and 16 scenes morph into each other. Controls in `examples/drone/README.md`. |
| `groove` | A six-track groovebox: synthesized kick, snare and hat, an acid bass, a lead and an LPC voice track, with the 16 keys as 16 steps. Controls in `examples/groove/README.md`. |
| `speak` | A talking typewriter: 267 words of LPC speech that can be said at any note, chained into a sentence and played back. Controls in `examples/speak/README.md`. |

## Rules the bootloader imposes

The build handles these; they matter if you change the linker script or packer.

- Linked at `0x08008000`, at most 98,288 bytes.
- `vector[0]` (initial SP) at most `0x20007FF8`: the bootloader pops 8 bytes from it.
- The trailer declares a length of at least `0x10000`; the checksum covers that range.
- Never write flash below `0x08008000`: the bootloader is not write-protected.

## Known pitfalls

- Give every hardware wait a time limit. An unbounded wait on the ADC left the synth dark and unresponsive.
- Look register bits up in RM0440 or the stock firmware, not from memory. The USB clock enable is `RCC_APB1ENR1` bit 23 on this chip; bit 26 (from another STM32 family) gave a device that never appeared on the bus.
- Poll the touch keyboard about every 10 ms and debounce it; read without a pause, it returns gaps.
- Do not draw to the display or use I2C from the audio callback.
- The simulator checks logic, not hardware timing: the ADC and stack-pointer faults only showed on the synth.

Details: `../docs/BOOTLOADER.md`, `../docs/HARDWARE.md`.

## Credits

`third_party/font8x8_basic.h`: public-domain 8×8 font by Daniel Hepper.
