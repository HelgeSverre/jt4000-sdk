# Prior work: reading a protected STM32G431, and Behringer firmware

Web research, 2026-11-21. Nothing here has been tried on the JT-4000. "Confirmed on" means the authors reported it on that chip; the STM32G4 is not confirmed for any of these.

**Update, same day:** the JT-4000 images are now decrypted without touching the chip. The cipher came from Music Tribe's PRO-800 packer and the key from its Windows build; see [FIRMWARE.md](FIRMWARE.md). The hardware approaches below are no longer needed for the application firmware; only the bootloader itself remains unread.

Summary at the time of research: no published method is confirmed on the STM32G4, and nobody had publicly decrypted a Behringer `*_CRY_*` image. The two closest reproducible results are flash erase suppression (STM32L422KBT6) and TraceRip (STM32G0).

## 1. Published work on RDP level 1, by closeness to the G4

| Work | Confirmed on | Equipment | G4 status |
|---|---|---|---|
| **Flash erase suppression**: Schink et al., TCHES 2021 (CVE-2021-29414), follow-up TCHES 2024, BSI/Fraunhofer study 2023 | STM32L422KBT6, STM32L1, plus Artery, Geehy, GigaDevice parts | ChipWhisperer-Pro or ChipSHOUTER; paper puts the setup at up to 6,500 USD | Plausible, untested. The L4 is the closest sibling (ST covers L4 and G4 protection in one note, AN4758) |
| **STM32-TraceRip**: Omo and Rowley, Hardwear.io USA, May 2025 | STM32F0/F1/G0/C0 | Pi Pico with modified OpenOCD; no fault injection | Plausible, untested. No Cortex-M4 parts tested |
| **Boot ROM fault injection**: Shaping the Glitch 2019, chip.fail 2019, Kraken 2020, SECGlitcher, Anvil | F1, F2, F3, F4 | Voltage fault-injection rig | Plausible, no G4 report found |
| **Exception(al) Failure** and related: Obermaier, Schink, Moczek, 2020 | STM32F103 and clones | SWD only | No evidence; TraceRip's authors couldn't reproduce it on the G0 |
| **SWD race, cold-boot stepping**: Obermaier and Tatschner, WOOT 2017 | STM32F0 | SWD plus power control | No evidence |

- **Flash erase suppression** interferes with the mass erase during an RDP 1→0 regression, so debug access returns with flash intact.
  - It needed both digital-domain buffer capacitors removed for the voltage variant; the electromagnetic variant needed no board changes.
  - The authors damaged many chips while searching for parameters. **A failed attempt erases everything.** On this synth that includes the bootloader, which can't be restored, so the unit would be bricked.
- **TraceRip** relies on core registers and SRAM staying readable under RDP 1; only flash is locked.
  - It power-cycles the chip, waits a precise delay, halts it, and reads the registers. Repeated across delays, this reconstructs what the code handled.
  - It needs target code that reads through flash at boot (their target computed a CRC over the application). Capture took days.
  - Inference, not published: the JT-4000 bootloader decrypts in software, so key material and plaintext pass through registers and SRAM during an update. Whether the debugger can attach at that point is unknown.
  - Source was promised at github.com/marcus-engineering; release not verified.

## 2. G4 protection behaviour (from ST documents)

- **RDP 1:** flash is blocked for the debugger, and also when booting from SRAM or the ROM bootloader. Code booted from user flash keeps full access.
- **Regression to level 0:** mass-erases flash, and clears CCM-SRAM and the backup registers.
- **Securable memory area** (`SEC_SIZE`): boot code can hide a flash region until the next reset. A bootloader key stored there is unreachable even to the running application.
- **PCROP:** execute-only regions; `PCROP_RDP` decides whether they are wiped on regression.
- **ROM bootloader** (AN2606/AN3156, generic): Read Memory is refused under RDP 1, and Readout Unprotect mass-erases.
- **Errata:** no RDP-related entry found in ES0430 by text search.

## 3. Behringer / Music Tribe

| Product | What's known |
|---|---|
| JT-4000 | A Mod Wiggler thread (Feb 2024) has someone fetching the 1.1.5 binary and stopping at the encryption and the locked chip (search snippet only; page returned 403) |
| SynthHUB (github.com/utajum/SynthHUB) | SysEx/WebUSB editor that lists the JT-4000. Its README says firmware update is out of scope |
| PRO-800 (STM32F446) | Music Tribe published `PRO800-Firmware-Packer` (packer binaries, schematic, memory map; Oct 2026). Bootloader at `0x08000000`, write-protected; updates over SysEx. Unknown whether the packer shares anything with the `CRY` scheme |
| Neutron (STM32F051) | Stock ROM DFU bootloader; factory firmware reportedly dumped with DfuSe Demo, so no read protection there |
| DEQ2496, BCR2000 | XOR cipher. On the DEQ2496 the keys sat in the bootloader and were recovered by disassembling it (Malvineous/behringerctl) |
| TD-3, Crave, Pro-1 | `.syx` updates; only the pattern/SysEx protocols are documented |
| SynthTribe | No reverse-engineering write-up found |

The PRO-800 packer is the most promising lead: it is the vendor's own tool for building update files for a synth in the same SynthTribe protocol family (`General_ProtocolZS` has PRO-800 handlers).

## 4. Comparable cases elsewhere

- **ST-Link** (lujji, 2016): the decryption routine was in the PC updater.
- **EEVblog automotive tool:** AES keys were located from S-box constants in a partial bootloader dump.
- **Korg Kronos** (marcan, 2016): keys came from a root shell on a Linux system; not transferable.
- **Arturia KeyStep, Novation:** updates aren't encrypted with a secret key.
- **Elektron Octatrack:** encrypted; key extraction unsolved.

No case was found that combines an encrypted update, a custom STM32 bootloader and a successful read-protection bypass on a synth.

## Sources

- https://penthertz.com/blog/breaking-stm32-readout-protection-from-uv-light-to-cpu-state-tracing/
- https://hardwear.io/wp-content/uploads/2026/04/TraceRip-Hardwear.pdf
- https://eprint.iacr.org/2021/640
- https://www.aisec.fraunhofer.de/content/dam/aisec/Dokumente/Publikationen/Studien_TechReports/englisch/Study-on-Hardware-Attacks-against-Microcontrollers.pdf
- https://nvd.nist.gov/vuln/detail/CVE-2021-29414
- https://blog.zapb.de/stm32f1-exceptional-failure/
- https://www.usenix.org/system/files/woot20-paper-obermaier.pdf
- https://www.usenix.org/system/files/conference/woot17/woot17-paper-obermaier.pdf
- https://tches.iacr.org/index.php/TCHES/article/view/7390
- https://blog.kraken.com/product/security/kraken-identifies-critical-flaw-in-trezor-hardware-wallets
- https://sec-consult.com/blog/detail/secglitcher-part-1-reproducible-voltage-glitching-on-stm32-microcontrollers/
- https://www.anvilsecure.com/blog/glitching-stm32-read-out-protection-with-voltage-fault-injection.html
- https://github.com/utajum/SynthHUB
- https://github.com/MusicTribeOpenSource/PRO800-Firmware-Packer
- https://github.com/klaine/pro-8000
- https://github.com/Malvineous/behringerctl/blob/master/doc/behringer-deq2496.md
- https://github.com/TheKikGen/BCR2000-dev
- https://marcan.st/2016/06/hacking-and-upgrading-the-korg-kronos/
- https://lujji.github.io/blog/reverse-engineering-stlink-firmware-part2/
- https://www.eevblog.com/forum/microcontrollers/dumping-stm32-protected-firmware/
- https://www.modwiggler.com/forum/viewtopic.php?t=259289&start=30 (snippet only)
