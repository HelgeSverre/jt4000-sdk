# Roadmap: understanding and replacing the JT-4000 firmware

Status 2026-11-21 (end of day): done through custom firmware. The images are decrypted, the bootloader is dumped and documented (`BOOTLOADER.md`), `tools/flash.py` replaces SynthTribe, and `sdk/` builds working firmware (`examples/trance` confirmed on hardware). The text below is the plan as written that morning, kept for the reasoning.

## What the 1.1.5 image shows already

| Fact | Evidence |
|---|---|
| Bare-metal Cortex-M4 image linked at `0x08008000`, about 93.5 KiB used of 96 KiB | vector table, trailer length `0x16D44` |
| Probably an RTOS | SVC, PendSV and SysTick handlers are all populated (the FreeRTOS pattern) |
| Interrupts used | DMA1 channel 1, USB low-priority, TIM3, TIM7, one more at IRQ 94 |
| Peripherals referenced | ADC1/2, DAC1, DMA1, I2C1, SPI1, USART2, USB, IWDG (watchdog), FLASH, and timers TIM1/2/3/6/7/8/15/16/17 |
| Integrity | trailer = length + byte-sum + `ACEFACEF`; no signature |
| Text | menu strings, USB descriptors ("JT-4000 MICRO", "MIDI Interface") in plain ASCII |

The many timers fit a design where timers generate the oscillator waveforms or clocks, with the analog filter outside the MCU.

## How the M-VAVE FM-1 scene does it (for comparison)

- The vendor published the factory firmware and an SDK (GPL-3.0, `m-vave/fm1_open`, October 2026) after the community had already built custom firmware.
- Firmware is written in **C**, built with the chip vendor's compiler (JieLi PI32 clang), because the FM-1 uses a JieLi Bluetooth audio chip with no mainstream toolchain.
- Community firmwares share a base project and install over USB from a browser page.
- A browser emulator (`fm1-emu`) lets people develop without the hardware.

What carries over: a shared base project (board support + audio skeleton), a safe USB install path, and an emulator. What doesn't: the JT-4000 has no vendor SDK or source, so the board has to be mapped by reverse engineering.

## Toolchain: is "compile our own" sensible?

Yes. The STM32G431 is one of the best-supported microcontrollers there is, far easier than the FM-1's chip.

| Option | Fit | Notes |
|---|---|---|
| **C with `arm-none-eabi-gcc` + ST's CMSIS/LL headers** | best default | smallest binaries, every STM32 example applies, easy to mix with code lifted from the original |
| **Rust (`embassy-stm32` or `stm32g4xx-hal`)** | good | G4 support is mature; safer concurrency for audio + UI; slightly larger binaries |
| Zig, C++ | workable | fewer ready-made drivers |
| MicroPython, JavaScript, TinyGo | poor | 96 KiB flash and 32 KiB RAM leave no room for a runtime plus a 4-voice audio engine |

Recommendation: C or Rust, producing a `.bin` linked at `0x08008000`, then one packaging step: pad to 98,304 bytes, write the trailer, encrypt with `tools/fwcrypt.py`. A higher-level language than that doesn't fit the chip.

## Phases

1. **Map the stock firmware (no risk).** Load 1.1.5 in Ghidra (`ghidra/`) with the STM32G431 register map (SVD). Label: SysEx handler, preset load/save, EEPROM driver, OLED driver, key scanning, audio engine. Diff 1.1.1→1.1.5 to see what Behringer changed. Output: `docs/FIRMWARE-MAP.md`, and the preset byte layout.
2. **Board map (no risk).** From the code plus continuity checks: which pin does what (OLED, EEPROM, pots, touch keys, DAC, filter control, MIDI in). Output: a pin table in `docs/HARDWARE.md`.
3. **Bootloader behaviour (the gating question).** Find out, from the app's "reboot to DFU" path and from SynthTribe's "Device is in DFU mode" handling:
   - how the bootloader decides to stay in update mode (the trailer? a RAM flag? a pin?),
   - whether there is a way into update mode that doesn't depend on a working application.
   Until there is a recovery path that survives a broken app, flashing anything is a brick risk, because SWD is read-protected and can't re-flash without erasing the bootloader.
4. **Round-trip test (low risk once 3 is answered).** Re-encrypt the unmodified 1.1.5 plaintext and confirm it is byte-identical to Behringer's file. Then flash an image with a one-byte cosmetic change (a menu string) through SynthTribe's own DFU path.
5. **Patches on the stock firmware.** Small changes with high value: more SysEx (edit-buffer dump, parameter set), CC coverage for the missing parameters.
6. **From-scratch firmware.** Board support package, then OLED + controls, then audio. Use an emulator (Renode or QEMU have partial G4 support) and ideally a second, sacrificial unit.

## Progress

- Phase 1 started: `ghidra/` project, SysEx handler, EEPROM layout and preset validation are in [FIRMWARE-MAP.md](FIRMWARE-MAP.md).
- Phase 3, partly answered: update mode is requested by writing `0xAA` to EEPROM address `0x7FC0` and resetting. The flag is in the external EEPROM, so it could be set with an I2C programmer if the application is broken. Unknown: what the bootloader does with a clear flag and an invalid application.
- Phase 4, offline half done: re-encrypting each decrypted image reproduces Behringer's file byte for byte.

- Phase 3, app side read (see "What the application tells us about the bootloader" in FIRMWARE-MAP.md): the app neither reads nor clears the update flag and never checks the trailer, so both jobs belong to the bootloader. Remaining unknowns need observation: a watched stock update, and an I2C capture of the EEPROM at power-up.

- **Phase 4 done (2026-11-21): a modified image runs.** `firmware/custom/JT-4000_INTFLASH_CRY_TEST1.bin` (splash text `JT-4000 MICRO` → `JT-4001 MICRO`, trailer byte-sum recomputed, re-encrypted with `tools/fwpack.py`) was flashed through SynthTribe's normal update, with Proxyman serving the file and its MD5. The synth booted showing `JT-4001 MICRO`. So the bootloader accepts images that Behringer didn't build: there is no signature. Not yet known: whether it checks the byte-sum at all (the test image had a correct one). Log: `captures/synthtribe-update-test1.log`.
- The flashing pipeline for custom images: three Proxyman scripting rules (version → 1.1.6 + checksum; `1.1.6/60` → `1.1.5/60`; serve local file), left **disabled** in Proxyman. Enable them and point the file path at the image to flash.

- **Phase 3 answered (2026-11-21): the bootloader is read out and its boot decision is known**, see [BOOTLOADER.md](BOOTLOADER.md). An invalid image keeps the synth in update mode, and holding two controls (PB4 and PA10 low) at power-on forces update mode. Flashing custom images is therefore recoverable. Packaging rules: `length ≥ 0x10000` and covering all code, byte-sum over the first `length` bytes, marker `ACEFACEF`.

- **Own flasher works (2026-11-21):** `tools/flash.py` flashes an image in about 3 seconds over the bootloader's serial protocol, with no SynthTribe or Proxyman. Update mode is entered by SysEx command `02` or by holding OSC + SETTINGS at power-on (both verified). The Proxyman rules are disabled and no longer needed.

- **Phase 6 started (2026-11-21): first from-scratch firmware runs.** `sdk/` builds a C image with `arm-none-eabi-gcc`; `make flash` packages and flashes it. `examples/hello` shows text and a counter on the OLED and reads the OSC and SETTINGS buttons. `jt_enter_update_mode()` returns to the bootloader from running code. `tools/emu.py` runs an image on the host and renders the display. Key lesson: the bootloader pops 8 bytes from the application's initial stack, see [BOOTLOADER.md](BOOTLOADER.md).

- SDK inputs and lights added (2026-11-21): LED/key chip, ADC and encoder drivers, verified with `examples/panel`. See "Panel controls" in [HARDWARE.md](HARDWARE.md).

- **First sound from custom firmware (2026-11-21).** `sdk/examples/tone`: an oscillator through the analog filter, played from the touch keyboard, with cutoff, resonance, pitch (SELECT) and waveform (OSC) all working. See "Sound path" in [HARDWARE.md](HARDWARE.md).

- **SDK at full speed with a playable voice (2026-11-21).** 170 MHz clock (inherited from the bootloader, or brought up by `jt_clock_170mhz()`; both timed on hardware), FPU on, audio rendered in the DMA interrupt in 2 ms blocks, 16-key keyboard read over I2C2, knobs read by a free-running ADC scan with DMA. Two bugs found on the way: the first ADC routine lagged one request behind, which had hidden a rotated channel table; and the keyboard chip must be polled slowly and debounced.

## Risks

- **Bricking:** see phase 3. A second unit is cheap insurance (about 50 EUR).
- **Bootloader assumptions:** the trailer check, flash write-protection and update-mode entry are inferred from the PRO-800's published behaviour, not verified on the JT-4000.
- **Licensing:** the stock firmware is Behringer's; share patches or tools, not their binaries.
