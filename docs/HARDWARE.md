# JT-4000 Micro hardware

Sources: the owner's teardown photos (`teardown-front.jpg`, `teardown-back.jpg`) and Gary Judge's teardown video "Cracking Open The Behringer JT-4000 Micro" (YouTube `vTID3qX17Ms`). Frames were taken from the 1080p stream with `yt-dlp` + `ffmpeg`. The photo blog linked from the video (3dims.co.uk) returned 403 and has an expired TLS certificate.

Board: `musictribe 0722-ABR`, `PCB303022REVD_01`.

## Main parts

| Ref | Part | Evidence | Notes |
|---|---|---|---|
| U1 | **STM32G431KBT6** (Cortex-M4F, 170 MHz, 128 KiB flash, 32 KiB SRAM, LQFP32) | marking `STM32G` / `431KBT6` (clear in the Reddit photo, see below); the video calls it "Arm Cortex-M4" | 128 KiB matches the 32 KiB bootloader + 96 KiB image. No hardware AES (that's the G441), so the firmware cipher runs in software |
| Y1 | crystal | next to U1 | |
| U4 | SOIC-8, `2429 SOND…` | near U1 | The 32 KiB EEPROM (I2C `0x50`, confirmed by capture and by the SDK's `jt_store_*`): presets, settings, update flag; layout in FIRMWARE-MAP.md |
| U7 | SOIC-8 | right side, between the pots | |
| U8 | SOT-23 | next to U1 | regulator or supervisor? |
| U13 | QFN, small | under the cutoff/resonance pots (video) | The video can't identify it; near the analog filter |
| U5, U9, U10 | TSSOP / SOIC-16 | back, along the keyboard edge | Likely touch-key scanning / shift registers |
| U6 | SOIC-8 | back, centre | |
| — | 0.96" OLED + HT-series driver | front, FPC | |

The photos this was read from are not in the repository.

### Community findings

Reddit r/synthdiy thread "Behringer JT4000 Micro PCB - Any idea what the solder points on the bottom left do?" (post `19elfjr`, 2024; full-resolution photos at `preview.redd.it/wort6qmy1fec1.jpg`, `…/qzc5m69z1fec1.jpg`):

- Commenters identify X4 as the MCU programming header. One repair technician notes that Behringer labels its test points `TP…`, so X4 isn't one of them.
- **u/Bi0hAzArD105:** "I was able to connect it using a cheap stlink v2 but it's read protected." That means SWD on X4 works and **RDP is at level 1**: the debugger connects (so not RDP 2), but flash reads are blocked. Re-checked on 2026-11-21: the option bytes read RDP level 1 (see "Debug probe query").
- The keyboard section is held to the case by strong double-sided tape, and two screws hold the PCB.

## Debug access

- **X4:** 6-pad header at the bottom-left edge of the front side; pad 1 is square, on the left, with a ▲ marker below it. SWD over X4 works (Reddit report above, and the debug probe query below). Pads 1–4 are identified below; pads 5 and 6 are unknown, and no pad has been traced to U1 with a continuity meter. On the LQFP32 G431, PA13 = SWDIO and PA14 = SWCLK.
- **U1 pins to trace X4 against** (LQFP32, datasheet DS12589 section 4.2; pin 1 is at the dot):

  | Pin | Signal | Use |
  |---|---|---|
  | 1, 17 | VDD | 3.3 V |
  | 4 | PG10-NRST | reset |
  | 21, 22 | PA11, PA12 | USB D−, D+ |
  | 23 | PA13 | SWDIO |
  | 24 | PA14 | SWCLK |
  | 31 | PB8-BOOT0 | boot mode select |
  | 32 | VSS | ground |

- **TP3, TP5:** test points next to U1. **TP7, TP10, TP1, TP2:** elsewhere on the board.


### X4 pad voltages (Saleae Logic 8, 2026-11-21)

Capture `captures/x4-poweron.sal`: analog + digital on all six pads through power-on, ground on the USB shell.

| Pad | Powered | Reading | Assignment |
|---|---|---|---|
| 1 (square) | 3.28 V, rises first | supply | **VDD** (measured) |
| 2 | 0.01 V, up to 0.10 V | weakly low, not hard ground | **SWCLK** (confirmed by the debug probe connecting) |
| 3 | −0.03 V, flat | hard ground | **GND** (measured) |
| 4 | 3.16 V, 0.5 ms after pad 1 | slightly under VDD: a pulled-up line | **SWDIO** (fits the pin's internal pull-up) |
| 5 | 0.01 V | no activity | unknown. Not an active-low reset held low, since the synth booted. Possibly unconnected, or the lead wasn't touching |
| 6 | 0.01 V | no activity while the display redrew | unknown; not the display clock |

Pads 1–4 match ST's standard layout (VDD, SWCLK, GND, SWDIO). Pads 2, 5 and 6 read alike, so the voltages alone don't identify pad 2. The debug probe in the next section confirmed it as SWCLK.

### Debug probe query (Raspberry Pi Debug Probe + OpenOCD 0.12.0, 2026-11-21)

Wiring: probe D port, black → pad 3, orange (clock) → pad 2, yellow (data) → pad 4. This confirms **pad 2 = SWCLK and pad 4 = SWDIO**. Read-only register reads, no reset, no halt requested:

| Register | Value | Decoded |
|---|---|---|
| SWD DPIDR | `0x2BA01477` | Cortex-M4 debug port |
| `DBGMCU_IDCODE` | `0x20026468` | device ID `0x468` = STM32G431/G441 |
| `FLASH_OPTR` | `0xFBEFF8BB` | **RDP byte `0xBB` = level 1**; `nSWBOOT0 = 0`, `nBOOT0 = 1` |
| `FLASH_WRP1AR`, `WRP1BR` | `0xFFC0FFFF` both | start page `0x7F` > end page `0x40`: **no write-protected area**, the bootloader included |
| `FLASH_PCROP1SR` / `1ER` | `0xFFFFFFFF` / `0x7FFFC000` | start > end: no execute-only (PCROP) area |
| `FLASH_SEC1R` | `0xFFFEFF80` | size bits `[6:0] = 0`: no securable (hidden) area; `BOOT_LOCK = 0` |

An RDP byte of `AA` would be level 0 and `CC` level 2. With `nSWBOOT0 = 0` and `nBOOT0 = 1` the BOOT0 pin is ignored and the chip always boots from main flash, so ST's ROM bootloader can't be entered by a pin. This also frees PB8 for the EEPROM clock.

OpenOCD reported "clearing lockup after double fault": the application crashed when the debugger attached, as expected under RDP 1 (flash access is cut off). A power cycle restores it.

What follows from this:

- Update mode is Behringer's own bootloader, not ST's ROM one (which is unreachable with these option bits).
- With no securable area and no PCROP, **code running from flash can read the whole flash**. The bootloader was read out this way on 2026-11-21: a patched stock application, installed through the normal update path, sent it over USB with no protection bypass (`BOOTLOADER.md`).
- With no write protection, a faulty application could also overwrite the bootloader. Custom code must never write below `0x08008000`.

## Panel controls (verified with `sdk/examples/panel`, 2026-11-21)

| Control | How the MCU sees it | Status |
|---|---|---|
| OSC button | PB4, low when pressed | verified |
| SETTINGS button | PA10, low when pressed | verified |
| LFO, FILTER, AMP, ARP buttons | LED/key chip (I2C `0x70`), key byte 0 bits 0–3 | verified: each lights its own LED in the demo |
| SELECT knob, press | key byte 0 bit 5 (`0x20`) | verified |
| SELECT knob, turn | quadrature on PA0/PA1, TIM2 encoder mode | verified: about 100 counts per half turn |
| Six button LEDs | LED/key chip display byte 4, bits 0–5 = OSC, LFO, FILTER, AMP, ARP, SETTINGS; red | verified |
| Cutoff pot | ADC1 channel 3 (PA2) | verified |
| Resonance pot | ADC1 channel 4 (PA3) | verified |
| Touch keyboard, key-down line | ADC1 channel 15 (PB0): 4080–4095 idle, 51–54 while any key is touched, the same for every key | verified as a key-down line only; the key number comes from the chip at I2C `0x65` (see "Sound path") |
| Volume | no ADC input reacts | analog only |

- **Correction (same day):** these three ADC assignments were first recorded rotated (PA2 keyboard, PA3 cutoff, PB0 resonance). The SDK's first ADC routine returned, on each call, the channel requested by the previous call, and the test programs happened to read in an order that cancelled the error. The continuous DMA scan in `sdk/src/adc.c` has no such lag, and the table above is from that.
- Display byte 6 of the LED/key chip, which the stock start-up animation also writes, has no visible effect.
- The LED/key chip accepts HT16K33 commands (`21` oscillator on, `81` display on, register `40` key data). Stock also sends `F2`, which is not an HT16K33 command; meaning unknown.

## Sound path (verified with `sdk/examples/tone`, 2026-11-21)

```
MCU DAC1 ch1 (PA4, 12-bit, 32 kHz) ──► analog filter ──► VOLUME ──► output
                                          ▲      ▲
              TIM8 CH3N (PB5) cutoff PWM ─┘      └─ TIM4 CH1 (PB6) resonance PWM (inverted)
```

| Item | Detail | Status |
|---|---|---|
| Audio out | DAC1 channel 1 on PA4; stock streams it by DMA from a 128-sample buffer, timed by TIM6 at 32 kHz | verified: sawtooth and square audible |
| Cutoff | PWM compare `value × 699 / 1000` on TIM8 channel 3, complementary output on PB5 | verified: the knob sweeps dull to bright as expected |
| Resonance | PWM compare `(1000 − value) × 699 / 1000` on TIM4 channel 1 on PB6 | verified: adds emphasis and **self-oscillates at maximum**, which is a property of the filter, not a fault |
| PWM rate | about 22 kHz in the SDK (timer clock 16 MHz, period 700); the stock code computes about 61 kHz | 22 kHz verified at both 16 and 170 MHz core clock: no whine or clicks. The stock rate is untested with SDK code |
| Amplifier | no separate control found: level is set digitally before the DAC | from the tone test: a digital fade gives clean note on/off |
| Touch keyboard | a second chip on I2C2 (PA8 SDA, PA9 SCL) at address `0x65`; a plain 2-byte read gives one bit per key (stock: `0x0801084C`) | verified: lowest key = `0x8000`, each key up is the next bit down, highest = `0x0001`; `0x0000` with nothing touched |

- Touch-keyboard key order, low to high: B, C, C♯, D, D♯, E, F, F♯, G, G♯, A, A♯, B, C, C♯, D (from the panel photo).
- At high resonance the tone takes on an edge. The owner's read is that this is the filter folding the wave, not clipping in the digital path (the test signal is at half scale).

## USB (verified with `sdk/src/usb.c`, 2026-11-21)

| Item | Value |
|---|---|
| Peripheral | STM32 USB full-speed device at `0x40005C00`, packet memory at `0x40006000`, interrupt 20 (USB_LP) |
| Pins | PA11 / PA12; no pin setup needed, the pull-up is internal (`USB_BCDR` bit 15) |
| Clock | 48 MHz internal oscillator (`RCC_CRRCR`), trimmed from the host's start-of-frame by the CRS; enable bit is `RCC_APB1ENR1` bit 23 (the stock firmware sets the same bit at `0x0800C12C`) |
| Stock identity | `1397:127D`, "JT-4000 MICRO", USB-MIDI with a 101-byte configuration, bulk endpoints `0x01` (out) and `0x81` (in), 64-byte packets |
| Bootloader identity | `1397:127E`, a serial port (`BOOTLOADER.md`) |
| SDK identity | `1397:127D`, "JT-4000 SDK", same interface layout |

The stock descriptors sit in compressed initialised data, so only fragments were read; the SDK uses the standard USB-MIDI layout that matches those fragments.

## Pin map (from firmware 1.1.5)

Read from the firmware's pin-setup code (`HAL_GPIO_Init` at `0x0800B2A8` and its callers). Not yet checked with a meter.

| Pin | Function | Evidence |
|---|---|---|
| PA0, PA1 | TIM2 CH1/CH2 (AF1) | SELECT knob encoder (verified) |
| PA2 | ADC1 channel 3 | cutoff pot |
| PA3 | ADC1 channel 4 | resonance pot |
| PB0 | ADC1 channel 15 | touch-keyboard key-down line |
| PA4 | DAC1 OUT1, fed by DMA1 channel 1 | audio output, 12-bit (verified) |
| PA5 | OLED reset (output) | pulsed low then high at display start-up |
| PA6 | OLED chip select (output) | toggled around every SPI transfer |
| PA15 | OLED data/command (output) | set differently for command and data writes |
| PA7 | SPI1 MOSI (AF5) | `HAL_SPI_MspInit`; OLED data |
| PB3 | SPI1 SCK (AF5) | OLED clock |
| PB7, PB8 | I2C1 SDA, SCL (AF4, open drain) | EEPROM U4 (`0x50`) **and** an HT16K33-style LED/key chip (`0x70`), both seen on U4 pins 5/6 in an I2C capture. PB8 is also the BOOT0 pin |
| PA8, PA9 | I2C2 SDA, SCL (AF4, pull-up) | touch-keyboard controller at `0x65`: 2 bytes, one bit per key |
| PB6 | TIM4 CH1 (AF2) | filter resonance PWM (verified) |
| PB5 | TIM8 CH3N (AF3) | filter cutoff PWM (verified) |
| PB4 | input, low = pressed | **OSC** button. Also read by the bootloader |
| PA10 | input with pull-up, low = pressed | **SETTINGS** button. Also read by the bootloader: OSC + SETTINGS at power-on forces update mode |
| PA11, PA12 | USB D−, D+ | USB device |
| PA13, PA14 | SWDIO, SWCLK | debug header X4 (not reconfigured by the firmware) |

- The OLED is on SPI (write-only: no MISO pin is set up), not I2C.
- The EEPROM shares I2C1 with the LED/key chip, which the application polls every 170 ms. An external programmer must stop the MCU first (debug probe halt), or its transfers will collide.

## JT-4000M differences (from firmware 1.1.6)

The JT-4000M Micro adds a MIDI input socket. Its firmware has the same image format, key,
application address and reset vector as the JT-4000 Micro's (`tools/fwfetch.py --m`), and its
pin setup differs in three places. Read from the decompiled 1.1.6 image
(`firmware/decrypted/JT-4000M_V116.bin`, base `0x08008000`); **not checked on a JT-4000M**.

| Pin | JT-4000 Micro (1.1.5) | JT-4000M (1.1.6) | Evidence in 1.1.6 |
|---|---|---|---|
| PA15 | OLED data/command (output) | USART2 RX, alternate function 7: the MIDI socket | pin setup at `0x0800DB9C` |
| PB0 | ADC1 channel 15: keyboard key-down line | OLED data/command (output) | `gpio_init` at `0x0800EB78`; display writes at `0x080185F4` (command) and `0x08018630` (data) |
| Keyboard key-down line | PB0 | not read; the ADC setup configures PA2 and PA3 only | ADC pin setup at `0x0800AC4C` |

| MIDI socket | Value |
|---|---|
| Peripheral | USART2 (`0x40004400`), 31250 baud, 8 data bits, no parity (`0x0800EEE4`) |
| Direction | Receive only: no transmit pin is set up |
| Transfer | Circular DMA from the receive register into a 64-byte buffer: DMA request 26, interrupt 12 (DMA1 channel 2) |

| USB | JT-4000 Micro | JT-4000M | Source |
|---|---|---|---|
| Application | `1397:127D` | `1397:12C7` | device descriptor in each image; SynthTribe's `config.json` |
| Bootloader | `1397:127E` | `1397:12C8` | SynthTribe's `config.json`; the Micro's verified |
| SysEx model bytes | `00 01 38` | `00 01 38` | SynthTribe's `config.json` |

- The 32-pin MCU has no spare pin, so the socket took PA15 and the display line took PB0 (inference).
- With addresses and calls masked, 85.7% of the 1.1.6 image also occurs in 1.1.5; 1.1.5 against 1.1.4 gives 83% on the same measure.
- Not known: the JT-4000M bootloader's info string, and whether it enters update mode with the same two buttons.

### How the SDK tells the two apart

One SDK image runs on both. `jt_gpio_init` reads PA15 and PB0 as inputs with the internal pull-downs before it drives either pin (`detect_board` in `sdk/src/board.c`):

| | PA15 | PB0 |
|---|---|---|
| JT-4000 Micro | low: only the display's input is on the line | high: the key-down line idles high; low while a key is touched |
| JT-4000M | high: a MIDI line idles high and is high in every stop bit | low: only the display's input is on the line |

The image treats the synth as a JT-4000M only when PA15 was seen high and PB0 was never high in 64 readings over about 4 ms. A JT-4000 Micro mistaken for a JT-4000M would drive PB0 against the keyboard chip's output; a JT-4000M mistaken for a Micro would drive PA15 against the MIDI receiver. The PA15 and PB0 levels in the table are inference from the two firmwares' pin setups; neither has been measured.

**Verified on a JT-4000 Micro (2026-11-24):** `sdk/examples/tone` built with this test was flashed; the display, touch keys and knobs worked, so the synth was taken for a Micro. The JT-4000M row is untested.

## Reference documents

Not stored in the repository; `docs/manuals/README.md` lists where to download each one (st.com, 2026-11-21):

| File | Why it matters |
|---|---|
| `STM32G431KB_datasheet_DS12589.pdf` | Pinout, memory map, electrical limits |
| `RM0440_STM32G4_reference_manual.pdf` | Flash option bytes, RDP levels, securable memory area, PCROP |
| `ES0430_STM32G431_errata.pdf` | Device errata; a text search found no protection-related entry |
| `AN2606_system_bootloader.pdf` | ST's ROM bootloader: entry conditions and behaviour per family |
| `AN3156_USB_DFU_protocol.pdf`, `UM0412_DfuSe_…pdf` | The DfuSe protocol in SynthTribe's `STM32USBUpdater`; not used for this synth (FIRMWARE.md) |
| `AN5156_STM32_security_intro.pdf` | ST's overview of protection features and threat model |
| `UM2262_SBSFU_…pdf`, `AN5056_SBSFU_integration_guide.pdf` | ST's reference secure bootloader (AES-based); a template many vendors start from |
| `AN2557_IAP_using_USART.pdf` | ST's classic in-application-programming bootloader example |

## How this fits the firmware findings

| Firmware fact | Hardware match |
|---|---|
| Image at `0x08008000`, 98,304 bytes | G431KB flash `0x08000000`–`0x0801FFFF`: 32 KiB bootloader + 96 KiB app = 128 KiB |
| SynthTribe flashes over a USB serial port with ST's UART-bootloader-style protocol (`STM32UARTUpdater`) | The G4 has a USB FS device. Behringer's own bootloader presents the serial port and accepts the encrypted image |
| Speck-64/128 in CBC mode | The G431 has no AES unit, so the cipher runs in software |
| `bootAddress 0x2403FFFC` in `config.json` | **Doesn't fit the G431** (SRAM is at `0x20000000`, 32 KiB). Probably copied from another product's template |

## Next steps (non-destructive first)

Step 2 was done on 2026-11-21 (RDP level 1, no securable area, `BOOT_LOCK = 0`; see "Debug probe query"). Step 4's SWD pads were confirmed by the same probe.

1. Trace X4 to U1 with a continuity meter (power off).
2. Connect an ST-Link (SWD) and **read the option bytes only**. The RDP level can be read even when flash is protected:
   - RDP 0: flash (bootloader + key) readable.
   - RDP 1 (`0xBB`-style "any other value"): debugger attaches, flash reads blocked.
   - RDP 2 (`0xCC`): debug permanently disabled.
   - Also check `SEC_SIZE` / `BOOT_LOCK`: the G4 "securable memory area" can hide the bootloader after reset.
3. **Never** change RDP from 1 to 0 ("regression"). It triggers a mass erase, deleting the bootloader and bricking the synth's update path.
4. A Saleae can confirm the SWD lines (SWCLK/SWDIO activity at power-on is rare) and find a debug UART on SWO or the unknown X4 pad.
