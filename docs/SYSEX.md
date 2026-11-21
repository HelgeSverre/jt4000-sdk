# JT-4000 Micro SysEx protocol

How SynthTribe talks to the Behringer JT-4000 Micro, recovered from the SynthTribe macOS app and checked against a real device.

| | |
|---|---|
| Device | Behringer JT-4000 MICRO, firmware 1.1.5, USB-MIDI port `JT-4000 MICRO` |
| App | SynthTribe 2.9.0 macOS (`com.behringer.synthtool`), x86_64 Mach-O, Qt 5, symbols not stripped |
| Tools | `otool -tV` (Xcode), radare2 6.2.2, `nm`/`c++filt`, mido 1.3.3 + python-rtmidi 1.5.8 |
| Test tool | [`tools/sysex.py`](../tools/sysex.py) |

> **Update:** the firmware is now decrypted and the full command list is read from its SysEx handler; see [FIRMWARE-MAP.md](FIRMWARE-MAP.md). Corrections to this document: the argument of `00 <n>` (n < `20`) is ignored and always dumps the selected preset; payload `02` is not "read operation mode" on this synth but **reboots it into firmware-update mode**.

Status legend: **verified** = sent to the device and the reply was checked; **app only** = found in SynthTribe, not tested on the JT-4000.

## Transport

All device communication goes over **USB-MIDI SysEx** through CoreMIDI (`MIDISend`). USB itself is used only for detection: SynthTribe registers IOKit matching notifications for the device's USB product IDs and then opens the MIDI port. It makes no vendor-specific USB requests.

Device definition: `SYNTHTRIBE.app/Contents/Resources/Devices/JT-4000 MICRO/config.json`

| Key | Value | Meaning |
|---|---|---|
| `listpid` | `0x127D`, `0x127E` | USB product IDs |
| `protocol` | `2` | `ProtocolVer` passed to `General_ProtocolZS::registerDevice` (clamped to max 3) |
| `modelid` | `0x00 0x01 0x38` | Three model bytes in every frame |
| `deviceid` | `0x00` | Only sent in frames when protocol = 3 |
| `cloudfamily` / `cloudmodel` | `jt-4000-micro` | Firmware API path, see [FIRMWARE.md](FIRMWARE.md) |
| `functions[].settings` | `dumpreceive`, subtype 1 | Shows the preset Receive panel |
| `functions[]` Update | `INTAddress 0x08008000`, `bootAddress 0x2403FFFC` | Firmware flash address (STM32-style memory map) |

A second entry in the same file, `JT-4000M MICRO`, has PIDs `0x12C8`/`0x12C7` and cloud model `jt-4000m`.

## Frame format

Built by `General_SysExParser::packMessage`:

```
 0     1  2  3     4  5  6     [7]       n         n+1
+----+----------+----------+---------+-----------+----+
| F0 | 00 20 32 | 00 01 38 | dev id  | payload…  | F7 |
+----+----------+----------+---------+-----------+----+
       Behringer  model ID   protocol 3  cmd + args
       / Music    (JT-4000)  only
       Tribe
```

- The JT-4000 uses protocol 2, so it has **no device-ID byte**: the payload starts at offset 7.
- `payload[0]` is the command. A reply usually uses command + 1.
- All bytes after `F0` are 7-bit (`00`–`7F`).
- `packMessageNoDeviceID` is a second framing, used by the file-dump commands (`7F 74 07 …`).
- The JT-4000 sends no reply to commands it doesn't support, and no error message either.

## Verified commands

### `08` Get firmware version

```
-> F0 00 20 32 00 01 38 08 F7
<- F0 00 20 32 00 01 38 09 00 01 01 05 F7
                        |  |  |  |  +- patch  5
                        |  |  |  +---- minor  1
                        |  |  +------- major  1
                        |  +---------- unknown (00; likely device id / status)
                        +------------- reply cmd 09
```

- Request: `"getfwver"` in `General_ProtocolZS::processRequest` calls `packageReadDeviceFirmwareVersion`, which builds payload `[08]`. Protocols other than 2 use `[08 00]`.
- Reply: the `0x09` entry in the `General_ProtocolZS` constructor's dispatch table calls `recvDeviceFirmwareVersion`. That handler expects 4 bytes after the command and reads indices 1–3 into `setFWversion(major, minor, patch)`.
- SynthTribe compares the result with the cloud's `latest` version to decide whether to offer an update.

### `04` Get model code

```
-> F0 00 20 32 00 01 38 04 F7
<- F0 00 20 32 00 01 38 05 50 30 45 4A 57 00 F7
                        |  "P  0  E  J  W" 00
                        +- reply cmd 05
```

The meaning of the code isn't known.

### `00 00` Receive Active Preset

```
-> F0 00 20 32 00 01 38 00 00 F7
<- F0 00 20 32 00 01 38 15 <64 preset bytes> 00 <cs> F7      (75 bytes)
                        |                    |   +- checksum
                        |                    +----- unknown (slot?)
                        +-- reply cmd 15
```

### `00 20` Receive All Presets

```
-> F0 00 20 32 00 01 38 00 20 F7
<- F0 00 20 32 00 01 38 10 <32 x 64 preset bytes> <cs> F7    (2058 bytes)
                        +-- reply cmd 10
```

All 32 presets arrive in a single SysEx message.

**Checksum:** `cs = (-sum(preset bytes)) & 0x7F`, so that `(sum + cs) & 0x7F == 0`. The `00` byte in the Active Preset reply doesn't affect the sum. This was checked on both dumps:

| Dump | Sum & 7F | cs |
|---|---|---|
| Active Preset | `35` | `4B` |
| All Presets | `0F` | `71` |

### Preset record (64 bytes)

```
offset 0                                          54  55 ............ 63
       +--------------------------------------------+------------------+
       | parameters (not yet mapped)                | name, 9 x ASCII  |
       +--------------------------------------------+------------------+
```

- The name occupies bytes 55–63, padded with spaces, for example `FATSAW␠␠␠`.
- The data is not 7-bit packed: every value is already ≤ `7F`.
- The Active Preset record is byte-identical to slot 0 of All Presets (FATSAW was the active preset).
- **The dump holds the stored preset, not the edit buffer.** `tools/ccmap.py` sent all 26 CCs from the guide (channel 1, values 0/32/64/96/127) and dumped after each one: no byte changed. This matches community notes that "PROG SING SYX SEND" sends the *saved* patch. Mapping the bytes therefore needs saved edits, not live CCs. CC 74 was also sent on each of the 16 channels, with a dump after each: no change on any channel. **Confirmed:** with TXCH/RXCH both set to 1, sending CC 74 = 0 on channel 1 made the display show the `*` (unsaved edit) marker, yet the dump stays unchanged. CCs edit the unsaved buffer, and both dump requests return saved data only.
- While SynthTribe is running it also receives these replies and opens a "save dump" dialog for each one. Quit SynthTribe before scripting dumps.

#### Byte layout: first guess (unconfirmed)

Based on the value range of each byte across the 32 factory presets, compared with the panel ranges in the Quick Start Guide. Every row needs confirming by saving a one-parameter edit and diffing the dump.

| Offset | Range seen | Guess |
|---|---|---|
| 0 | 1–7 | OSC1 wave (0 Off, 1 tri, 2 square, 3 PWM, 4 saw, 5 supersaw, 6 FM, 7 noise); FATSAW = 5 |
| 1 | 0–5 | OSC2 wave (0 Off, 1 tri, 2 square, 3 PWM, 4 saw, 5 noise) |
| 2 | 0–99 | OSC1 PWM / supersaw detune / FM feedback |
| 3 | 0–99 | OSC2 PWM |
| 4 | 0–24, mostly 12 | OSC1 coarse tune (12 = centre) |
| 5 | 0–19 | OSC1 fine tune? |
| 6 | 0–24 | OSC2 coarse tune (12 = centre) |
| 7 | 0–35 | OSC2 fine tune or OSC balance? |
| 8–11 | always 0 | unused |
| 12–22 | 0–99 | envelopes, VCF and LFO amounts: 11 bytes in the 0–99 range. 20 is almost always 0 or 99, so likely a sustain level |
| 23–42 | always 0 | unused / reserved |
| 43, 45, 48, 53 | 0–1 / 0–2 | switches (ring mod on/off, VCF trigger, portamento mode, LFO destination?) |
| 44, 49, 50 | 0–99 | ring mod amount, portamento time? |
| 46 | 0–31 | ? |
| 47 | 0–2 | LFO wave (tri/square/saw)? |
| 51, 52 | 0–35 | LFO rates? |
| 54 | always 0 | name prefix / padding |
| 55–63 | ASCII | name, 9 characters |

#### Existing third-party editors

As of 2026-11-21, none of them document or parse the synth's dump format:

| Project | What it has |
|---|---|
| [chriswareham/jt4000](https://github.com/chriswareham/jt4000) (Java) | CC editor; its patch file uses its own Yamaha-style (`F0 43 00 7A …`) container, not the synth's format |
| [SpeakingColors/jt4000-vst](https://github.com/SpeakingColors/jt4000-vst) | CC map credited to midi.guide (CC BY-SA); says the SysEx format is undocumented |
| [nuppeppou/JT4000M](https://github.com/nuppeppou/JT4000M) | Web MIDI CC editor, `sysex: false` |
| [gary909/JT4000M-MIDI-EDITOR](https://github.com/gary909/JT4000M-MIDI-EDITOR), [bfugitt/jt4000](https://github.com/bfugitt/jt4000) | No SysEx code |

The midi.guide list adds CCs that aren't in the guide: 29 OSC balance, 47 VCF envelope amount, 54/55 LFO1/LFO2 wave, 56 LFO1 destination. These are useful labels for the remaining bytes.

Factory slots 0–31 on the test unit: FATSAW, SUPULSE, KRAFT, WARMFIFTH, SEQ5, VINTSTRNG, RUBBASS, BASS, PORTBASS, TRANCYPAD, 12DBGLORY, BRIGHTSTR, REZOPAD, SOFT, RINGBASS, PIVUEMME, FASTPORTA, BASSOON, DEEPSYNTH, VIBRA, SLOW, SYNBASS, STRANGE, GLISR', AVTODA, OP3RA, DRAC, OLDVYNIL, WIND, SNARE, FM-BELL, PARKUTI.

Make your own with `uv run tools/sysex.py dump presets -o dumps/all-presets.syx` (`dumps/` is git-ignored).

### Where the dump requests come from

`qml/general/SynthToolControlDumpReceive.qml` (compiled into the binary) shows Receive buttons that call `generalController.requestReceive(n)`. `General_GeneralController::requestReceive(int)` builds the frame by hand: header, model bytes, then a jump table on `n`:

| n | Button | Payload | JT-4000 |
|---|---|---|---|
| 0 | Active Preset | `00 00` | reply `15`, verified |
| 1 | All Presets | `00 20` | reply `10`, verified |
| 2 | Active Pattern | `01 00` | no reply |
| 3 | All Patterns | `01 10` | no reply |

Model `00 01 31` (another device) gets extra `77 01` / `77 00` follow-up requests in cases 0 and 1. Models `00 01 33` and `00 01 62` take separate paths in case 1.

### Send side (upload)

The panel's **Send** buttons call `generalController.dumpReceive(n)`. That stores `n` in `this+0x46c` and opens a file dialog; `onLoadDumpReceive(path)` then does the upload:

1. Read the whole `.syx` file.
2. Reject it ("Wrong file!") unless it is `F0 00 20 32 <m0 m1 m2> … F7` with this device's model bytes.
3. Check the command byte at file offset 7 against the button:

   | n | Button | Accepted command |
   |---|---|---|
   | 0 | Active Preset | `15`–`17` |
   | 1 | All Presets | `10`–`12` |
   | 2 | (pattern) | `25` |

4. Send the file **unchanged** as `rawdata`, in 255-byte chunks. Model `00 01 62` has its own path, which checks for a size of `0x1080` bytes.

An upload is therefore just a dump reply sent back to the synth. The UI tells the user: "Please enable Sysex Receive mode in System Menu".

**Tested (2026-11-21):**

- **Receive mode is armed by viewing the menu item.** Uploads are accepted only while SETTINGS → SYSEX RECEIVE is on screen; there's no separate "arm" action. Each upload shows "SYSEX RECEIVE SUCCESS". After a reboot the item first showed "WRONG FILE", probably the status from the last receive (unconfirmed). Uploads sent from any other screen are dropped silently.
- **A cmd `15` (single preset) upload overwrites the saved active slot, not just the edit buffer.** FATSAW with its name changed to `TESTSAW` showed up on the display, in the next Active Preset dump, and in slot 1 of the All Presets dump.
- The checksum was recomputed for the edited frame; it's unknown whether the synth checks it.

> `00 00` has the same bytes as `SetDeviceId(0)` (`00 <id>`, see below). The JT-4000 treats it as a dump request.

## Tested, no reply

| Payload | App meaning |
|---|---|
| `06` | Read model name |
| `01 00`, `01 10` | Active / All Patterns |
| `75` | Read entire parameter |
| `77 00`, `77 00 00`, `77 01 00 00` | Read sequencer / JT-MINI preset / bank |

## Command table (app only)

Extracted from every `General_SysExParser::package*` builder by reading the constant bytes stored before the `packMessage` call. These builders are shared by all Behringer synths in SynthTribe, so most of them probably don't apply to the JT-4000. `<a>` marks a value taken from an argument.

| Payload | Builder |
|---|---|
| `00 <id>` | SetDeviceId |
| `02` | ReadDeviceOperationMode → reply `03`? (table lists `recvDeviceOperationMode`) |
| `03 <mode>` | SetDeviceOperationMode |
| `03 10` / `03 00` | StartUserCVCalibration / ExitCVCalibration |
| `04` | ReadDeviceModelCode → `05` |
| `06` | ReadDeviceModelName → `07` |
| `08 [<a>]` | ReadDeviceFirmwareVersion → `09` |
| `0C <a> <b>` | GetCVValue → `0D` |
| `0D …` | SetCVValue, SavePress (`0D 40 00…`), RestoreCVCalibration (`0D 20 00…`) |
| `0E <a> <b> <c> [<d> <e>]` | SetMIDIChn |
| `0F <a>` | SetMIDIInTranspose |
| `10 <a> <b> <c>` | SetVelocity |
| `11 <a> <b>` | SetPitchBend / SetPortamentoPitchBend |
| `12 <a>` | SetKeyPriority |
| `13 <a>` | SetClockDivision / SetArpClockDivision |
| `14 …` | SetRetrig / SetMultiTriggerInfo |
| `15 <a>` | SetModulationCurve |
| `16 <a>` | SetNoteZeroCV |
| `17 <a>` | SetMIDIClockOutput |
| `18 <a>` | SetMIDIVelocityOutput |
| `19 …` | SetSyncPolarity / SetSyncPolaritySeparate / SetClockOutAndInPolarity |
| `1A <a>` / `1B <a>` | SetSyncClockType / SetSyncClockSource |
| `1C <a>` | SetAccentThreshold / SetExternalClockPPQN |
| `1D <a>` | SetSyncInEnable / SetMIDIClockControl |
| `1E <a>` / `1F <a>` | SetTempoValue / SetAssignValue |
| `20`–`27 <a>` | ModWheelRange, Output ModWheel/PitchWheel/Keyboard/AfterTouch/Sequencer/Arpeggiator, GlideMode |
| `28 <a> <b> <c>` | SetMidiThrough |
| `29 …` / `2D …` | SetVoiceNoteMap / SetPreference |
| `2A`, `2B`, `2C`, `2E`, `2F`, `30`, `32` | ChainSongs, EnhancedMode, TriggerOutSource, CVRange, LocalKBMode(Ex), KBVELOutRange, ARPRepeatMode |
| `74 <sub> …` | Extended settings: `00` AutoPowerOff / TrigTime, `01` TempoRange / AccentMode / AccentOffLevel, `02` SplitValue / MidiShortMessage, `03` JoyPressRetrigger, `04` ModWheel / DecayKill, `05` NewSeq, `06` BypassGate / RhythmLogic, `08` Quantize, `5F` GetSlotsName |
| `75` | ReadEntireParameter → `76` |
| `76 …` | SetEntireParameter / SetWaveSettingData |
| `77 …` | Read by address / sequencer / preset (`77 01 <a> <b>` JT-MINI), bank, samples (`77 30`, `77 31`) |
| `78 …` | Set sequencer / preset / bank / LM-DRUM samples (`78 10`, `78 11`) |
| `79 <a>` | SetMIDIThru |
| `7A`–`7C`, `7E` | Poly chain: max channel, set, voice mode, get |
| `7D` | RestoreFactorySettings (**destructive**) |
| `7F 74 07 01 7F "BIN " …` | File dump header (no-device-ID framing) |
| `7F 74 07 02 …` | File dump data |
| `7F 74 07 03 7F "BIN " …` | File dump request |
| `F0 7E 00 7B 00 F7` | File dump EOF (universal SysEx) |
| `… 03 30` | RebootDFU: two frames, reboots into the firmware-update bootloader (**do not send casually**) |

Reply dispatch table (`General_ProtocolZS` constructor): `05` model code, `07` model name, `09` firmware version, `0D` CV value, `5A` old update, `76` entire parameter, `78` sequencer parameter, `7E` poly chain. `recvAck` and `recvDeviceOperationMode` are also registered.

## Open questions

- Map the 64 preset bytes to parameters. Method: change one knob, dump, diff.
- Find the meaning of `00` in the firmware reply and `00` before the checksum in the Active Preset reply.
- Confirm that an upload works in SYSEX RECEIVE mode, and whether a `15` frame goes to the edit buffer or the stored slot. Then map bytes by uploading single-byte edits.
- Find the DFU and update flow: `sendFirmwareData`, `RebootDFU`, and how `JT-4000_INTFLASH_CRY_*.bin` reaches the device.
