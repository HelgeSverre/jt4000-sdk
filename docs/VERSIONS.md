# Firmware version differences

Compared with `ghidra/scripts/verdiff.py <old> <new>`, which matches functions between two decompilations after masking addresses and variable names. Counts are approximate: a function that only moved still shows as changed if it embeds table addresses. Behringer publishes no release notes for these (the cloud's `notes` field is a single space), so the descriptions below are read from the code and strings.

| Step | Functions identical | Size (trailer length) |
|---|---|---|
| 1.1.1 → 1.1.2 | 380 of 397 | `0x16F38` → `0x16F30` |
| 1.1.2 → 1.1.4 | 365 of 397/400 | `0x16F30` → `0x16AF0` |
| 1.1.4 → 1.1.5 | 366 of 400 | `0x16AF0` → `0x16D44` |

## 1.1.1 → 1.1.2 (2023-09-22): one routine rewritten

- Version constant 1.1.1 → 1.1.2.
- One routine at `0x08013CE4` shrank from about 850 to 235 characters of C, and a new 3.3 KB routine appeared at `0x08015E88`: logic moved or rewritten.
- No string changes. Same initial stack pointer and reset vector.

## 1.1.2 → 1.1.4 (2023-12-07): menus and MIDI handling rewritten

- Menus reorganised. Removed: `OCT A:SET`, `MOD A:SET`. Added: `KEY A:OCT B:HOLD`, `ARP A:SET`, `CHO A:ENAB B:SET` (a chord mode and a hold function).
- MIDI handling rewritten. The channel-message handler grew from about 1 KB to 9.4 KB of C (`0x0800E8D4` → `0x0800E9DC`; `midi_handle_message` at `0x0800EB3C` in 1.1.5). This is most likely where the CC map in the Quick Start Guide was added or extended (inference).
- A note-range check appeared (`21 ≤ note ≤ 103`, or `0xFF`).
- LFO wave name `SAWT` removed from the string table.
- The menu routine grew by about 12%. Initial stack pointer changed (`0x20007D40` → `0x20007F38`): more static RAM in use.
- No 1.1.3 was published.

## 1.1.4 → 1.1.5 (2024-01-29): oscillator balance

- New page `OSC A:BAL` with the `1&2 1<NN>2` display: the oscillator balance parameter (preset byte 8). This explains why every factory preset has 0 in that byte: the presets predate the parameter.
- Changes in two audio routines near `0x0800AE1C`.
- Version constant 1.1.4 → 1.1.5.

## What didn't change

- The image trailer format (length, byte-sum, `ACEFACEF`) is the same in all four versions (verified).
- The SysEx handler (same `F0 00 20 32 00 01 38` check) and the update-flag write to EEPROM `0x7FC0` are present in 1.1.1 as well. They weren't compared line by line across versions.
