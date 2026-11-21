# JT-4000 Micro SDK (early)

Build custom firmware for the Behringer JT-4000 Micro in C and flash it over USB, with no
SynthTribe and no debug probe.

Status: verified on real hardware at the full 170 MHz clock: start-up, display, all six
buttons and their lights, the SELECT knob, cutoff and resonance pots, the 16-key touch
keyboard, interrupt-driven audio out, the analog filter and USB MIDI in both directions.
The sound blocks and menu are confirmed through `examples/trance`, live drawing through
`examples/soundbox`. Not yet: preset storage helpers, DIN MIDI.

## Requirements

- `arm-none-eabi-gcc` and `arm-none-eabi-binutils` (Homebrew formulas of the same names)
- `uv` (runs the packaging, flashing and simulator tools in `../tools`)

## Use

```
make                    # build examples/hello -> build/hello/hello.cry.bin
make flash              # build and flash (about 3 seconds)
make sim                # run in the simulator and draw the display
make new APP=myapp      # start examples/myapp from examples/hello
make APP=myapp flash
uv run --with unicorn ../tools/emu.py build/tone/tone.bin --key --wav out.wav   # capture the DAC
```

Flashing needs the synth in update mode. `make flash` gets there by itself when the
synth runs stock firmware, or SDK firmware that calls `jt_usb_init()` and `jt_poll_escape()`. Otherwise hold
**OSC + SETTINGS** while switching the synth on.

To return to stock: `uv run ../tools/flash.py flash ../firmware/JT-4000_INTFLASH_CRY_V115.bin`.

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
| `jt_audio_init(render)` | 12-bit DAC at 32 kHz; `render(out, n)` is called from an interrupt for each 2 ms block |
| `jt_filter(cutoff, resonance)` | the analog filter, each 0..1000 |
| `jt_usb_init()` | class-compliant USB-MIDI device "JT-4000 SDK" (`1397:127D`), runs from the USB interrupt |
| `jt_midi_get(ev)` | next received USB-MIDI event (4 bytes), false when none |
| `jt_midi_send(status, d1, d2)`, `jt_midi_send_sysex(msg, n)` | send to the host |
| `jt_poll_escape()` | call in the main loop: OSC + SETTINGS held, or the update-mode SysEx received over USB → update mode |
| `jt_enter_update_mode()` | set the bootloader's EEPROM flag and reset |
| `jt_reset()` | system reset |

## Sound blocks (`include/jt_dsp.h`)

Float, one struct and one `next` function per block, no C library.

| Block | Purpose |
|---|---|
| `jt_osc_t` | saw, square (both band-limited), triangle, sine, noise |
| `jt_env_t` | attack, decay, sustain, release |
| `jt_lfo_t` | sine, triangle, square, random steps |
| `jt_svf_t` | digital low/band/high-pass filter, stable while swept |
| `jt_tape_t` | a loop of recent audio in your own buffer: delays, and the source for grains |
| `jt_grains_t` | four overlapping slices of the tape replayed at another pitch |
| `jt_reverb_t` | four cross-fed delay lines, 5 KiB, tuned for a 16 kHz call rate |
| `jt_exp2`, `jt_note_hz`, `jt_sin`, `jt_rand`, `jt_softclip` | maths helpers |

The DAC feeds the analog filter, so digital effects sit *before* it: closing the cutoff knob also closes the reverb tail.

## Menu (`include/jt_menu.h`)

Describe up to six pages (one per panel button) as tables of items that point at your `int16_t` variables; call `jt_menu_poll()` every 10 ms. Turn SELECT to choose, press to edit. See `examples/trance`.

## Examples

| Example | Shows |
|---|---|
| `hello`, `panel`, `bringup` | display, inputs, start-up stages |
| `tone` | one oscillator, keyboard, USB MIDI notes and CCs |
| `soundbox` | an ambient sound box with no text: a generator plays notes from a scale, six buttons open six animated scenes, the two knobs shape the open one; echo, grains, reverb, boot animation |
| `trance` | seven-saw supersaw, trance gate, tempo-synced delay, presets, built-in riffs and arpeggiator that follow the held key; `tools/riff.py` imports MIDI files as extra riffs |

## Rules the bootloader imposes

The build handles these; they matter if you change the linker script or packer.

- Linked at `0x08008000`, at most 98,288 bytes.
- `vector[0]` (initial SP) at most `0x20007FF8`: the bootloader pops 8 bytes from it.
- The trailer declares a length of at least `0x10000`; the checksum covers that range.
- Never write flash below `0x08008000`: the bootloader is not write-protected.

## Things that bit us

- Give every hardware wait a time limit. An unbounded wait on the ADC turned a small bug into a dark, dead-looking synth.
- Look register bits up in RM0440 or the stock firmware; do not trust memory. The USB clock enable is `RCC_APB1ENR1` bit 23 on this chip; bit 26 (from another STM32 family) gave a device that never appeared on the bus.
- Poll the touch keyboard about every 10 ms and debounce it; read flat out, it returns gaps.
- Don't draw to the display or use I2C from the audio callback.
- The simulator checks logic, not hardware timing: the ADC and stack-pointer faults only showed on the synth.

Details: `../docs/BOOTLOADER.md`, `../docs/HARDWARE.md`.

## Credits

`third_party/font8x8_basic.h`: public-domain 8×8 font by Daniel Hepper.
