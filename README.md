# JT-4000 Micro: custom firmware

![JT-4000 custom firmware SDK: a dot orbiting on a pixel display](docs/header.gif)

Write your own firmware for the Behringer JT-4000 Micro in C and flash it over the synth's
own USB port in about three seconds. No SynthTribe, no debug probe, no opening the case.

This repository holds two things:

- **`sdk/`**: a small C SDK. Display, lights, buttons, knobs, touch keyboard, 32 kHz audio
  out, the analog filter, USB MIDI, sound building blocks (oscillators, envelopes, filter,
  delay, grains, reverb) and a menu system.
- **The reverse-engineering work behind it** (`docs/`, `tools/`, `ghidra/`): the firmware
  encryption, the bootloader's update protocol, the SysEx commands and the hardware map.

Unofficial. Not affiliated with or endorsed by Behringer / Music Tribe. You flash your
synth at your own risk; see [Safety](#safety-and-recovery) for why the risk is small.

## Quick start

Needs `arm-none-eabi-gcc`, `make` and [`uv`](https://docs.astral.sh/uv/). Developed and tested on macOS; nothing is macOS-specific except `fwfetch.py`'s default path to SynthTribe, but Linux and Windows are untried.

```sh
brew install arm-none-eabi-gcc arm-none-eabi-binutils uv     # macOS

cd sdk
make sim                      # build examples/hello and run it in the simulator
make flash                    # build and flash it to the synth
make APP=soundbox flash       # an ambient sound box: plays itself; six buttons, two knobs, pictures, no menus
make APP=trance flash         # supersaw, trance gate, built-in riffs
make APP=speak flash          # a talking typewriter: Speak & Spell style speech, sung at any note
make new APP=mysynth          # start your own from examples/hello
```

Connect the synth by USB and switch it on before `make flash`. On stock firmware, and on
SDK firmware, the tool restarts the synth into update mode by itself. If that fails, hold
**OSC + SETTINGS** while switching the synth on (the button lights blink) and run it again.

Back to the factory firmware at any time (from the repository root):

```sh
uv run tools/fwfetch.py          # download the latest factory firmware into firmware/
uv run tools/flash.py flash firmware/JT-4000_INTFLASH_CRY_V115.bin
```

The factory firmware is Behringer's and is not part of this repository. `fwfetch.py`
downloads it from Music Tribe's update server the way SynthTribe does. Fetch it once,
before you flash anything else, so the way back is on your disk.

## A firmware in twenty lines

```c
#include "jt4000.h"
#include "jt_dsp.h"

static jt_osc_t osc = {.wave = JT_WAVE_SAW};

static void render(int16_t *out, int n) {            /* called from the audio interrupt */
    for (int i = 0; i < n; i++) out[i] = (int16_t)(jt_osc_next(&osc) * 12000.0f);
}

int main(void) {
    jt_init();                                        /* clock, display, panel, knobs, keyboard */
    jt_usb_init();                                    /* USB MIDI; also lets `make flash` restart us */
    jt_audio_init(render);
    for (;;) {
        jt_poll_escape();                             /* OSC + SETTINGS -> update mode */
        int key = jt_keyboard_lowest(jt_keyboard());
        if (key >= 0) jt_osc_hz(&osc, jt_note_hz(47 + key), JT_SAMPLE_RATE);
        jt_filter(jt_pot(JT_ADC_CUTOFF) / 4, jt_pot(JT_ADC_RESONANCE) / 4);
        jt_delay_ms(10);
    }
}
```

The full API is in [`sdk/README.md`](sdk/README.md) and the three headers in `sdk/include/`.

## What you are working with

| | |
|---|---|
| Processor | STM32G431KBT6: Cortex-M4 with FPU, 170 MHz |
| Memory | 96 KiB of flash for your firmware, 32 KiB of RAM |
| Audio | 12-bit DAC, 32 kHz, mono, followed by the analog filter |
| Display | 128 × 32 OLED (16 × 4 characters) |
| Controls | 16 touch keys, 6 lit buttons, a push encoder, cutoff and resonance knobs |
| Connection | USB (MIDI when running, serial in update mode) |

Two consequences shape most designs. The analog filter comes **after** the DAC, so it
filters everything, digital effects included. And 32 KiB of RAM means about half a second
of delay memory at 16 kHz, not seconds of sampling.

Everything in the SDK has run on a real JT-4000. Not written yet: preset storage helpers and DIN MIDI.

## Safety and recovery

The factory bootloader lives in the first 32 KiB of flash and is never touched by flashing.
It checks the firmware image before starting it and offers three ways into update mode:

1. Hold **OSC + SETTINGS** while switching on. Always works, whatever is flashed.
2. An image that fails its check (interrupted flash) lands in update mode by itself.
3. Running firmware asks for it (the stock firmware over SysEx, SDK firmware the same way).

So a broken firmware costs a button combination, not a synth. The one thing that could
do real damage is code that writes to flash below `0x08008000`; the SDK contains none.

Your presets live in a separate memory chip. Custom firmware leaves it alone unless you
write to it; back up first, on stock firmware, with `uv run tools/sysex.py dump presets -o dumps/all-presets.syx`.

## Repository map

| Path | Content |
|---|---|
| `sdk/` | The SDK, its examples and its own README |
| `tools/` | `fwfetch.py` (download factory firmware), `flash.py` (flasher), `fwpack.py` (packaging, encryption), `emu.py` (simulator), `sysex.py` (talk to stock firmware), `riff.py` (MIDI file to riff for `examples/trance`), `lpcwords.py` (word list to LPC speech for `examples/speak`). They end with a one-line `error:` on failure; set `JT_DEBUG=1` for the traceback |
| `docs/` | `HARDWARE.md` (pins, parts), `BOOTLOADER.md` (start-up rules, update protocol), `FIRMWARE.md` (image format, encryption), `SYSEX.md`, `FIRMWARE-MAP.md` (stock firmware internals, preset format, CC map) |
| `firmware/` | Empty until you run `tools/fwfetch.py` (`--all --decrypt` for every version, decrypted, as the Ghidra scripts expect) |
| `ghidra/` | Scripts and named symbols for reading the stock firmware |
| `AGENTS.md` | Working rules for AI coding agents |

## Building with an AI agent

`AGENTS.md` tells a coding agent how to work in this repository: the build, simulate,
flash loop, the limits of the chip, and the mistakes already made once. Point your agent
at the repository and describe the instrument you want.

## Licence

MIT, see `LICENSE`. That covers this repository's code and documentation, not Behringer's
firmware.

## Credits

- 8×8 font: Daniel Hepper, public domain (`sdk/third_party/font8x8_basic.h`).
- Earlier work on the JT-4000 and related Behringer devices: see `docs/PRIOR-WORK.md`.
