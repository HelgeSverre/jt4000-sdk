# JT-4000 firmware map (application 1.1.5)

From the decrypted image `firmware/decrypted/JT-4000_V115.bin`, loaded at `0x08008000` and decompiled with Ghidra 12.1.4. Reproduce with `ghidra/analyze.sh 115`; output lands in `ghidra/out/v115/` (`decomp.c`, `functions.tsv`, `strings.tsv`). 400 functions; names below are Ghidra's defaults (`FUN_<address>`).

## SysEx handler: `FUN_08016f28`

Checks for `F0 00 20 32 00 01 38`, then switches on the command byte. **This is the complete list of commands the synth accepts:**

| Cmd | Args | Action | Reply |
|---|---|---|---|
| `00` | `< 20` | Dump the **currently selected** preset from EEPROM (`FUN_08017064`). The argument value is ignored | `15` + 64 bytes + `00` + checksum |
| `00` | `20` | Dump all 32 presets (`FUN_08017130`) | `10` + 2048 bytes + checksum |
| `02` | — | **Reboot into firmware-update mode** (`FUN_08012760`), see below | `03 00`, then model code |
| `04` | — | Model code | `05 "P0EJW" 00` |
| `08` | — | Firmware version | `09 00 01 01 05` (constant in the image) |
| `10` | 2048 bytes | Receive all presets: write `0x800` bytes at EEPROM offset 0 | — |
| `15` | 64 bytes | Receive one preset: write `0x40` bytes at EEPROM offset `current_preset × 64` | — |

- `10` and `15` are accepted only when the UI state is `menu == 6` and `item == 8`, which is the SETTINGS → SYSEX RECEIVE screen. Anything else sets a status value of 3 (shown as "WRONG FILE").
- There is no command for the edit buffer, single parameters, or a chosen preset slot. `06`, `01`, `75`, `77` don't exist, which matches the earlier no-reply tests.
- Checksum (`FUN_08017678`): `(-sum(bytes)) & 0x7F`, as measured.

### `02`: reboot into update mode

`FUN_08012760` does this:

1. Sends `F0 00 20 32 00 01 38 03 00 F7`.
2. Queues a write of one byte, **`0xAA`, to EEPROM address `0x7FC0`** (`FUN_080171fc`) and waits for it to finish.
3. Writes `0x05FA0004` to `SCB->AIRCR` (`0xE000ED0C`): a system reset.

So the "stay in update mode" flag lives in the **external I2C EEPROM**, not in the MCU. The bootloader presumably tests `EEPROM[0x7FC0] == 0xAA` at power-up and clears it after a successful update (not verified; the bootloader is unread).

> Sending `02` puts the synth into update mode until an update completes. Don't send it casually. `tools/sysex.py send 02` would do exactly that.

Consequence for custom firmware: there may be a recovery path that doesn't need the application or SWD. An I2C programmer clipped onto the EEPROM (U4) could set the flag by hand. Still to check: what the bootloader does when the flag is clear but the application is invalid.

## What the application tells us about the bootloader

Read from the 1.1.5 application, before the bootloader was dumped. What the bootloader really does is in `BOOTLOADER.md`, which wins where the two differ.

| Observation in the app | What it implies |
|---|---|
| `SystemInit` (`0x08011090`) enables the FPU and sets `VTOR = 0x08008000` itself | The bootloader just jumps to the app; the app doesn't depend on state the bootloader leaves behind |
| The only reference to EEPROM `0x7FC0` is the write of `0xAA` in `eeprom_queue_update_flag` | The app never reads or clears the flag, so the **bootloader clears it** (when is unknown: on entry, or after a successful download) |
| No code references the trailer marker `ACEFACEF`, the length or the checksum | Any image check is done by the bootloader, not the app |
| `FLASH` registers are touched only to set wait states during clock setup | The app never writes flash or option bytes |
| No watchdog (`IWDG`) access anywhere | The bootloader doesn't leave a watchdog running |
| Factory presets are stored in the app image and copied to the EEPROM by FACTORY RESET | A wiped EEPROM is recoverable from the menu |

USB identity in normal operation: vendor `0x1397` (Behringer), product `0x127D`, product string "JT-4000 MICRO". `config.json` lists a second product ID, `0x127E`, which is probably the update-mode identity (to confirm).

SynthTribe's updater (`STM32USBUpdater::prepareDevice`) claims the DFU interface, reads the ST-style memory-layout string descriptor, and has a conditional `DFUSE::ReadUnprotect` step guarded by a `m_bRemoveReadProtection` setting. Traced: the step runs only if byte `+0xE1` of the updater is non-zero, which is copied from `SettingItem+0x69`. `General_UpdateController::initial` never sets that byte (it sets `+0x6A` and `+0x6C`), and no device config has a key for it, so the read-unprotect step is **off** in the normal update flow. That matters because on ST's own ROM bootloader, read-unprotect erases the chip.

### Observed at power-on (I2C capture, 2026-11-21)

Saleae on U4 pins 5 (SDA) and 6 (SCL); capture `captures/u4-i2c-poweron.sal`, decoded in `captures/u4-i2c-poweron.csv`. Times are relative to the supply rising.

| Time | Device | Transfer | Meaning |
|---|---|---|---|
| +15 ms | `0x50` EEPROM | write `7F C0`, read 1 byte → `00` | **The bootloader reads the update flag** as its first bus action. The application never reads this address, so this is bootloader code |
| +145 ms | `0x70` | write `21 81 EF …` then 15 rounds of 18-byte writes, 0.2 s apart | Application start: HT16K33-style LED/key driver initialised (oscillator on, display on, brightness), then the button-LED start-up animation |
| +3.18 s | `0x50` EEPROM | read 64 bytes at `7F40` | Global settings |
| +3.19 s | `0x50` EEPROM | read 64 bytes at `0000` | Preset 1 |
| every 170 ms after | `0x70` | write `40`, read 6 bytes | Key-matrix scan |

- With the flag at `00` the bootloader makes no other EEPROM access and writes nothing.
- U4 is confirmed as the EEPROM (7-bit address `0x50`, 16-bit memory addresses).
- A second device at `0x70` shares this bus. Its commands match a Holtek HT16K33 (LED driver with key scan), which fits the "Holtek driver" seen in the teardown video. The routine named `i2c_read_6_bytes_reg40` is its key scan.

## EEPROM layout (I2C, device address `0xA0`, 16-bit addressing)

Read routine: `FUN_08016dc0(offset, buffer, length)`, in 64-byte pages with a 5 ms pause.

| Offset | Size | Content |
|---|---|---|
| `0x0000` | `0x800` | 32 presets × 64 bytes |
| `0x7F40` | `0x40` | global settings block (read at start-up) |
| `0x7FC0` | 1 | update-mode flag (`0xAA` = enter update mode) |

## MIDI CC map: `midi_handle_cc` (`FUN_08015600`)

The complete list of CCs the firmware acts on, with the preset byte each one writes. Value scaling is `value × range / 127` unless noted. Every CC sets the "edited" flag.

| CC | Parameter | Preset byte | In the Quick Start Guide? |
|---|---|---|---|
| 1 | Modulation | — | yes |
| 5 | Portamento amount | 46 | yes |
| 24 | OSC1 wave | 0 | yes |
| 25 | OSC2 wave | 1 | yes |
| 28 | LFO2 amount | 52 | yes |
| 29 | OSC balance | 8 | **no** |
| 47 | VCF envelope amount | 22 | **no** |
| 54 | LFO1 wave | 47 | **no** |
| 55 | LFO2 wave | 48 | **no** |
| 56 | LFO1 destination (≥ 64 = second option) | 53 | **no** |
| 64 | handled, no preset byte (sustain/hold) | — | **no** |
| 70 | LFO1 amount | 50 | yes |
| 71 | VCF resonance | 13 | yes |
| 72 | LFO1 speed | 49 | yes |
| 73 | LFO2 speed | 51 | yes |
| 74 | VCF cutoff | 12 | yes |
| 81–84 | VCA attack, decay, sustain, release | 18–21 | yes |
| 85–88 | VCF attack, decay, sustain, release | 14–17 | yes |
| 95 | Ring mod amount | 44 | yes |
| 96 | Ring mod switch | 43 | yes |
| 111, 112 | OSC1, OSC2 fine tune | 5, 7 | yes |
| 113, 114 | OSC1 PWM/detune/feedback, OSC2 PWM | 2, 3 | yes |
| 115, 116 | OSC1, OSC2 coarse tune | 4, 6 | yes |

- The CC handler and the menu code agree on every byte, which cross-checks the preset table below.
- The five undocumented CCs (29, 47, 54, 55, 56) match the community list on midi.guide. CC 22 for OSC2 wave, claimed by one web editor, is not handled; 25 is.
- Not reachable by CC: portamento mode (byte 45), the name, and all arpeggiator, chord and key settings.

## Preset record (64 bytes)

Named from two functions: the loader `preset_load_current` (`FUN_080151c8`), which range-checks each byte, and the menu code `ui_menu_update_and_draw` (`FUN_08008cec`), which prints each page's title and writes the edited value into a specific byte. Out-of-range bytes are replaced by the default on load.

| Offset | Parameter | Range | Default | Menu page |
|---|---|---|---|---|
| 0 | OSC1 wave: 0 OFF, 1 TRIANG, 2 SQUARE, 3 PWM, 4 SAW, 5 SUPSAW, 6 FM, 7 NOISE | 0–7 | 0 | `OSC A:WAVE` (1) |
| 1 | OSC2 wave: 0 OFF, 1 TRIANG, 2 SQUARE, 3 PWM, 4 SAW, 5 NOISE | 0–5 | 0 | `OSC A:WAVE` (2) |
| 2 | OSC1 PWM (wave 3) / supersaw detune (5) / FM feedback (6) | 0–99 | 0 | `B:PWM` / `B:DETU` / `B:FDBK` |
| 3 | OSC2 PWM (wave 3) | 0–99 | 0 | `B:PWM` |
| 4 | OSC1 coarse tune | 0–24 | 12 | `OSC A:COAR B:FINE` (1) |
| 5 | OSC1 fine tune | 0–100 | 0 | |
| 6 | OSC2 coarse tune | 0–24 | 12 | `OSC A:COAR B:FINE` (2) |
| 7 | OSC2 fine tune | 0–100 | 0 | |
| 8 | OSC balance, stored as `0x40 + value` (value 0–63, 31 = centre) | `40`–`7F` | `5F` | `OSC A:BAL` |
| 9–11 | unused | | | |
| 12 | VCF cutoff (confirmed: written by CC 74) | 0–99 | 99 | main screen |
| 13 | VCF resonance (confirmed: written by CC 71) | 0–99 | 0 | main screen |
| 14 | VCF envelope attack | 0–99 | 0 | `VCF A:ATT B:DEC` |
| 15 | VCF envelope decay | 0–99 | 0 | |
| 16 | VCF envelope sustain | 0–99 | 99 | `VCF A:SUST B:RELS` |
| 17 | VCF envelope release | 0–99 | 0 | |
| 18 | VCA envelope attack | 0–99 | 0 | `VCA A:ATT B:DEC` |
| 19 | VCA envelope decay | 0–99 | 0 | |
| 20 | VCA envelope sustain | 0–99 | 99 | `VCA A:SUST B:RELS` |
| 21 | VCA envelope release | 0–99 | 0 | |
| 22 | VCF envelope amount | 0–99 | 0 | `VCF A:AMT` |
| 23–42 | unused | | | |
| 43 | Ring mod switch (0 OFF, 1 ON) | 0–1 | 0 | `RING :AMOU B:SWIT` |
| 44 | Ring mod amount | 0–99 | 0 | |
| 45 | Portamento mode (0 PORT, 1 GLIS) | 0–1 | 0 | `PORTA:AMOU B:MODE` |
| 46 | Portamento amount | 0–99 | 0 | |
| 47 | LFO1 wave (TRIANG, SQUARE, …) | 0–2 | 0 | `LFO A:WAVE B:DEST` |
| 48 | LFO2 wave | 0–2 | 0 | `LFO A:WAVE` (2) |
| 49 | LFO1 speed | 0–99 | 0 | `LFO A:SPEE B:AMNT` (1) |
| 50 | LFO1 amount | 0–99 | 0 | |
| 51 | LFO2 speed | 0–99 | 0 | `LFO A:SPEE B:AMNT` (2) |
| 52 | LFO2 amount | 0–99 | 0 | |
| 53 | LFO1 destination | 0–1 | 0 | `LFO A:WAVE B:DEST` |
| 54 | unused | | | |
| 55–63 | Name, 9 ASCII characters | | | `PROG NAME EDIT` |

- Bytes 12 and 13 have no menu title (they are the dedicated knobs), so they were pinned through the CC handler instead: CC 74 writes byte 12 and CC 71 writes byte 13.
- All 32 factory presets store 0 in byte 8, which is out of range, so the balance loads as centre.
- Arpeggiator, chord, key and MIDI settings aren't in the preset. They live in the global settings block at EEPROM `0x7F40`.
- Check against FATSAW: byte 0 = 5 (SUPSAW), byte 2 = 52 (detune), byte 4 = 12 (coarse centre), bytes 16 and 20 = 99 (sustains).

## UI state (`0x2000092C`)

| Offset | Meaning |
|---|---|
| 0 | menu: 1 OSC, 2 LFO, 3 VCF, 4 VCA, 5 ARP, 6 SETTINGS |
| 2 | page within the menu |
| 7 | settings item (8 = SYSEX RECEIVE) |
| 10 | selected preset index, 0–31 |
| 11 | "edited" flag (the `*` on the display) |

## Vector table and peripherals

See [ROADMAP.md](ROADMAP.md) for the summary. Handlers: SVC, PendSV, SysTick (RTOS), DMA1 channel 1, USB low-priority, TIM3, TIM7, IRQ 94.
