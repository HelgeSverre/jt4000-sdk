# JT-4000 bootloader

The 32 KiB bootloader at `0x08000000`, read out of the MCU on 2026-11-21.

| | |
|---|---|
| File | `firmware/bootloader.bin`, 32,768 bytes, MD5 `2f1f4e5abd16185d587037892bdd006a` |
| Used | about `0x6981` bytes (26.4 KiB); the rest is `FF` |
| Strings | `BEHRINGER`, `JT-4000 MICRO BOOTLOADER`, `BE_P0EJW_BOOT9`, and the firmware key `-MakeLoveNotWar-` at offset `0x63A8` |
| Vectors | initial SP `0x20004610`, reset `0x080002B9` |
| Decompiled | `ghidra/analyze-boot.sh` → `ghidra/out/boot/` (238 functions) |

## How it was read

The chip is read-protected against the debug port (RDP level 1), but code running from flash can read all of flash, and no area is hidden (see [HARDWARE.md](HARDWARE.md)). So the stock application was patched to read flash on request:

- `patches/flashdump.py` redirects one call in `sysex_send_current_preset` to a 52-byte routine in the image's unused space. Request `F0 00 20 32 00 01 38 00 00 <lo7> <hi7> F7` returns 32 bytes at `0x08000000 + block × 32` as 64 nibbles.
- The image was flashed through SynthTribe's normal update (see [FIRMWARE.md](FIRMWARE.md)).
- `tools/flashdump.py verify` first read the application area back and compared it with the flashed image (match), then `read 0x08000000 0x8000` produced the dump.

## Boot decision: `main` at `0x080060A8`

In this order:

1. **Button check.** Reads PB4 and PA10. If **both are low** (both buttons held at power-on), go to update mode. In the application (`0x08015CC0`), PB4 low toggles menu 1 (OSC) and PA10 low toggles menu 6 (SETTINGS). So the combination is **OSC + SETTINGS held while switching on**. These two buttons are wired straight to the MCU; the other four menu buttons go through the I2C key driver.
2. **Checksum.** Reads the trailer at `0x0801FFF0`: `length`, `checksum`, `marker`. Sums the bytes from `0x08008000` up to index `length` and compares with `checksum`.
3. **Update flag.** Reads EEPROM `0x7FC0`. If it is `0xAA`: queue a write of `0x00` to clear it, wait for that write, then go to update mode.
4. **Start the application** only if all of these hold, otherwise go to update mode:
   - `0x10000 ≤ length ≤ 0x18000`
   - `marker == 0xEFACEFAC` (bytes `AC EF AC EF`)
   - the checksum matched

Consequences:

| Fact | Meaning for custom firmware |
|---|---|
| A bad checksum, marker or length keeps the synth in update mode | A corrupt or half-written image is recoverable by flashing again |
| The flag is cleared on *entering* update mode, before any download | An interrupted update doesn't loop on the flag; the bad checksum keeps it in update mode instead |
| The sum covers only the first `length` bytes | Code placed after `length` isn't covered. Stock images hide this because their padding is zero. Always set `length` to cover everything |
| `length` must be at least `0x10000` | A small custom image must still declare ≥ 64 KiB (pad with zeros) |
| Two-button entry is checked before anything else | A hardware way into update mode that needs neither a working application nor the EEPROM flag |
| No signature, no version check | Any correctly packaged image is accepted (confirmed twice by flashing) |

The remaining failure case is an image with a valid trailer whose code hangs before it can accept SysEx command `02`. The two-button entry (OSC + SETTINGS) covers that.

**Verified on the hardware (2026-11-21):** holding OSC + SETTINGS while switching on brought the synth up as USB device `1397:127E`, product string `JT-4000 MICRO BOOTLOADER`, serial port `/dev/cu.usbmodem<chip serial>1`, with no MIDI ports. `tools/flash.py info` then got `79`-acknowledged replies: chip ID `01 04 21` and info `BE_P0EJW_BOOT9`.

## Hand-over to the application: `0x08004290`

```
ldr  r0, [app, #4]        ; reset vector, saved to RAM
str  r0, [RCC, #0x18]=0   ; RCC interrupt enables cleared
str  app, [VTOR]          ; VTOR = 0x08008000
ldr  r0, [app, #0]
msr  MSP, r0              ; SP = the application's vector[0]
ldmia sp!, {r4, lr}       ; pops 8 bytes FROM THE NEW STACK
bx   <reset vector>
```

State on arrival: interrupts enabled, SysTick still firing every 1 ms, SYSCLK 170 MHz (HSE crystal → PLL, set up at `0x08004338`), `SP = vector[0] + 8`.

**The pop is a trap for custom firmware.** If `vector[0]` is the very end of RAM (`0x20008000`, the usual choice), the pop reads past the end of memory and the processor faults inside the bootloader. Nothing of the application runs, and the synth looks dead with the update-mode lights still blinking. Stock images avoid it only because their initial SP sits below the end of RAM (`0x20007E40` in 1.1.5).

Rule: **`vector[0]` must be at most `0x20007FF8`.** The SDK's linker script uses `0x20007FF0` and its reset handler loads SP itself. `tools/flash.py` and `tools/emu.py` refuse images that break the rule.

This cost five failed flashes on 2026-11-21. It was found by flashing probes that executed one start-up instruction each, then reading this routine's disassembly once even the first probe failed.

## Serial update protocol: handler at `0x08003C40`

In update mode the bootloader is a USB serial port (vendor ID `0x1397`). The protocol follows ST's AN3155 UART bootloader: each command is the byte plus its complement, `79` is ACK and `1F` is NACK.

| Command | Exchange | What the bootloader does |
|---|---|---|
| Sync | `7F` → `79` | |
| Get | `00 FF` → `79 06 31 00 02 11 21 31 44 79` | lists the commands |
| Get ID | `02 FD` → `79 01 04 21 79` | fixed reply |
| Info | `AA 55` → `79 0E <14 bytes> 79` | the ASCII string `BE_P0EJW_BOOT9` (verified on the device) |
| Read memory | `11 EE` … | **disabled**: answers with `AA` filler bytes instead of memory |
| Erase | `44 BB` → `79`, then `FF FF 00` → `79` | erases flash pages 16–63, the whole application area |
| Write | `31 CE` → `79`; 4 address bytes (big-endian) + XOR → `79`; `N−1`, N data bytes, XOR of all → `79` | address must be in `0x08008000`–`0x08020000`. The data is **decrypted in place** (Speck CBC), then programmed 8 bytes at a time |
| Go | `21 DE` → `79` | jumps to the application without re-checking the trailer |
| `BB 44` | | enters another routine (`0x080043F0`) that polls the buttons; looks like a hardware test mode. Not analysed |

- The CBC chaining state is initialised once, when the bootloader starts (`0x08006360`). An image therefore has to be written **once per bootloader session, in order, from `0x08008000`**, in multiples of 8 bytes. A retry needs the bootloader restarted first.
- Write does not erase; the Erase command must come first.
- SynthTribe writes 32-byte chunks. `tools/flash.py` does the same.
- What SynthTribe calls `eraseQSPI` in its log is this Erase command.

**Verified (2026-11-21):** `tools/flash.py flash firmware/JT-4000_INTFLASH_CRY_V115.bin` erased, wrote all 98,304 bytes in 2.9 s and started the application; the synth came back as a MIDI device reporting 1.1.5 with the normal preset-dump command restored. The serial port disappears as soon as Go is accepted, so the final ACK is usually not seen.

`tools/flash.py` implements this: `info` (enter update mode, print ID and info) and `flash <image>` (check the trailer the way the bootloader will, erase, write, go). It refuses images the bootloader would reject.

## Not yet analysed

- The `BB 44` routine.
- What the bootloader shows on the display in update mode.
