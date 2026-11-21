# JT-4000 Micro firmware distribution

How SynthTribe finds and downloads JT-4000 firmware. The traffic was captured with Proxyman on 2026-11-21 from SynthTribe 2.9.0 (macOS).

## Cloud API

Host: `https://cloud.midasconsoles.com/api/v2` (Music Tribe cloud, behind Cloudflare). The client sends `User-Agent: Mozilla/5.0` and `Accept: application/json`.

```
SynthTribe                                   cloud.midasconsoles.com
    |  POST /auth/tokens                              |
    |  {"client":"63dbaa87-15b1-4681-a47f-34ea5bfd10df"}
    |------------------------------------------------>|
    |  {client:{salt, proofSalt, proof}, responsePath}|
    |<------------------------------------------------|
    |  POST /auth/tokens/{jti}/challenge-response     |
    |  {"clientResponse":"<32-byte hex>"}             |
    |------------------------------------------------>|
    |  {success:true, token, jti, uuid:000…}          |
    |<------------------------------------------------|
    |  GET /firmware/{brand}/{family}/{model}/latest  |
    |  Authorization: <token>                         |
    |------------------------------------------------>|
```

- **Client ID:** `63dbaa87-…` is SynthTribe's `clientVersionId`. It is echoed back in every firmware record.
- **Handshake:** the server returns a 64-byte `salt`, a 64-byte `proofSalt` and a 32-byte `proof`. The client answers with a 32-byte response. Both sides hold a 512-byte secret (stored in the SynthTribe executable as 1,024 hex digits): `proof` = SHA-256(secret ‖ proofSalt) lets the client check the server, and `clientResponse` = SHA-256(secret ‖ salt). `tools/fwfetch.py` implements it and carries the secret.
- **Anonymous login:** `uuid` is all zeros, so no user account is needed.
- **Authorization required:** send `Authorization: Bearer <token>`; requests without it fail with `400 "Authorization header missing from request"`.

### Endpoints

| Method | Path | Result |
|---|---|---|
| GET | `/firmware/behringer/jt-4000-micro/jt-4000-micro/latest` | Newest record (object) |
| GET | `/firmware/behringer/jt-4000-micro/jt-4000-micro` | All records (array, newest first) |
| GET | `/firmware/behringer/jt-4000-micro/jt-4000-micro/{version}` | One record; 404 `"Record doesn't exist"` if missing |
| GET | `…/{version}/binary` | Binary (listed in the record, not exercised) |

The path segments come from `cloudfamily` and `cloudmodel` in the device's `config.json`. The JT-4000M variant uses `jt-4000m/jt-4000m`.

### Firmware record

```json
{
  "version": "1.1.5",
  "createdAt": "2024-01-29T22:56:33.287Z",
  "public": true, "deprecated": false,
  "brand": "behringer", "family": "jt-4000-micro", "model": "jt-4000-micro",
  "filename": "JT-4000_INTFLASH_CRY_V115.bin",
  "bytes": 98304,
  "checksum": "5d7bdd949a4d70c4a6e4d52cd1ed72ec",
  "binary": "https://cloud.midasconsoles.com/api/v2/firmware/…/1.1.5/binary",
  "s3Url": "https://mcloud-…oss-accelerate.aliyuncs.com/mcloud-service-firmware-go-prod/…?x-oss-…",
  "clientVersionId": "63dbaa87-…", "clientUUID": "00000000-…", "binaries": []
}
```

- `checksum` is the MD5 of the file.
- `s3Url` is a pre-signed Aliyun OSS link that uses temporary STS credentials and expires after about 55 minutes. It needs no further authentication.

## Released versions

| Version | Uploaded | File | Bytes | MD5 |
|---|---|---|---|---|
| 1.1.5 | 2024-01-29 | `JT-4000_INTFLASH_CRY_V115.bin` | 98,304 | `5d7bdd949a4d70c4a6e4d52cd1ed72ec` |
| 1.1.4 | 2023-12-07 | `JT-4000_INTFLASH_CRY_V114.bin` | 98,304 | `8e285be86bd0c6c67e7aba2375be8aad` |
| 1.1.2 | 2023-09-22 | `JT-4000_INTFLASH_CRY_V112.bin` | 98,304 | `b3928e160e0b7ac6b34a59c9612cb70b` |
| 1.1.1 | 2023-08-18 | `JT-4000_INTFLASH_CRY_V111.bin` | 98,304 | `694f981921b829927a97c6a1f77ad4e0` |

- There is no 1.1.3: the server returns 404.
- The bundled `config.json` still says `"lastVersion":"1.1.1"`.
- All four files are in `firmware/`, and each matches its MD5. The full API response is in `firmware/listing.json`.

## Image format

**Solved (2026-11-21).** The images are encrypted with **Speck-64/128 in CBC mode**:

| Item | Value |
|---|---|
| Cipher | Speck, 64-bit block, 128-bit key, 27 rounds |
| Mode | CBC; each block is two little-endian 32-bit words `(y, x)` |
| IV | `y = 0x11223344`, `x = 0x55667788` (bytes `44 33 22 11 88 77 66 55`) |
| Key | ASCII `-MakeLoveNotWar-` (`2d4d616b654c6f76654e6f745761722d`), as four little-endian words |
| Padding | none; the image is exactly 98,304 bytes |

How it was found:

1. Music Tribe published `MusicTribeOpenSource/PRO800-Firmware-Packer` (Oct 2026), a tool that builds PRO-800 update files. Its 14 KB Linux binary contains the cipher: key schedule at `0x1980`, CBC encrypt at `0x1ac0`, IV constant at `.rodata 0x2048`. The PRO-800 uses a random key per file, sent in the update header, so that binary holds no fixed key.
2. `tools/fwcrypt.py` reimplements the cipher (checked against the published Speck64/128 test vector).
3. Every 16-byte window of the files at hand was tried as the key. CBC decryption of blocks 1–2 doesn't depend on the IV, and should give four Cortex-M vectors (`0x0800xxxx`, low bit set). The **Windows** packer (`windows/PRO800_update_gen.exe`, offset `0x88040`) contains the string that decrypts all four JT-4000 images. SynthTribe itself contains no working key (15.2 M windows tried).

Decrypt: `uv run tools/fwcrypt.py try firmware/JT-4000_INTFLASH_CRY_V115.bin --key 2d4d616b654c6f76654e6f745761722d -o firmware/decrypted/JT-4000_V115.bin`

Decrypted images are in `firmware/decrypted/`:

| Version | Initial SP | Reset vector | Notes |
|---|---|---|---|
| 1.1.1 | `0x20007D40` | `0x080082C9` | |
| 1.1.2 | `0x20007D40` | `0x080082C9` | same first block as 1.1.1, which is why the ciphertexts share 8 bytes |
| 1.1.4 | `0x20007F38` | `0x080082C9` | SP changed, so the first ciphertext block differs; key and IV unchanged |
| 1.1.5 | `0x20007E40` | `0x080082C9` | |

- Valid Cortex-M vector table for a 32 KiB-RAM part linked at `0x08008000`, matching the STM32G431KB.
- **Trailer** (last 16 bytes, at image offset `0x17FF0`), verified on all four versions:

  ```
  0x17FF0  <length u32 LE>  <checksum u32 LE>  AC EF AC EF  00 00 00 00
  ```

  | Field | 1.1.5 value | Meaning |
  |---|---|---|
  | length | `0x16D44` | size of code + data; the rest up to the trailer is zero padding |
  | checksum | `0x00915534` | plain sum of all bytes in `[0, 0x17FF0)` |
  | marker | `ACEFACEF` | constant |

  The integrity check is a byte sum, not a signature. The bootloader checks it at every start and stays in update mode when it fails (`BOOTLOADER.md`).
- The PRO-800-style header (tag at `0x18`, length at `0x20`, CRC at `0x24`) is **not** used: those slots hold a vector and zeros.
- Menu strings are in plain text (`OSC A:WAVE B:PWM`, `PORTA:AMOU B:MODE`, `PROG NAME EDIT` …), so the preset layout can now be read from the code.

### Pre-decryption analysis (kept for the record)

All four images are 98,304 bytes (96 KiB). They are meant for internal flash at `0x08008000`, which leaves the first 32 KiB for a bootloader.

| Observation | Implication |
|---|---|
| Entropy 7.998 bits/byte; no vector table at offset 0 | Encrypted (`CRY` = crypted) |
| V111 and V112 share their first 8 bytes `98 20 41 23 23 d2 5b 91`, then differ | 64-bit block cipher, same key, ECB or fixed IV; identical plaintext start (vector table) |
| V114 and V115 share no prefix with V111/V112, or with each other | Key or IV changed from 1.1.4, or the start of the image changed |

### Cross-version diff

| Test | Result | Rules out |
|---|---|---|
| Repeated 8/16-byte blocks within a file | none in any file (12,288/12,288 unique) | ECB |
| V111 vs V112 at the same offset | bytes 0–7 equal; every later block fully different (block `0x08` XOR has no `00` bytes) | CTR/OFB/CFB (unchanged plaintext bytes would XOR to `00`) |
| XOR of every pair | entropy 7.998, `00` count at chance level (364–415) | keystream reuse |
| Blocks shared at any offset | none, apart from V111/V112 block 0 | re-use after code moves |
| Entropy per 4 KiB | 7.94–7.97 throughout | plaintext padding / tail |

Conclusion (inference): a **64-bit block cipher in CBC mode** (XTEA, Blowfish, DES/3DES class), with the same key and IV for V111 and V112. Their first plaintext block (initial SP + reset vector) is unchanged; the rest of the vector table changed. A 128-bit cipher such as AES would make a match of exactly 8 bytes vanishingly unlikely. V114 and V115 share no first block with any other version: the reset vector, the IV or the key changed, and the data can't tell which.

Chip: **STM32G431KBT6** (Cortex-M4, 128 KiB flash), identified from teardown photos; see [HARDWARE.md](HARDWARE.md). App at `0x08008000` + 98,304 bytes = exactly 128 KiB of flash with a 32 KiB bootloader. The G431 has no hardware AES, so the cipher runs in software. An earlier guess here (STM32H7, based on `bootAddress 0x2403FFFC`) was wrong: that address isn't in the G431's memory map and is probably left over from another product's config.

## Update path (SynthTribe → device): observed

**Correction (2026-11-21):** an earlier version of this section said the JT-4000 updates over USB DFU (DfuSe, libusb). A traced official update shows otherwise. The DfuSe code exists in SynthTribe, but for this synth the update runs over a **USB serial port** with ST's UART-bootloader-style protocol. A debugger breakpoint on `libusb_control_transfer` never fired during the whole update.

Observed sequence, re-flashing stock 1.1.5 (log: `captures/synthtribe-update-stock115.log`):

| Step | Log line | Meaning |
|---|---|---|
| 1 | `F0 00 20 32 00 01 38 02 F7` → reply `… 03 00 F7` | SysEx command `02`: the app sets the EEPROM flag and resets |
| 2 | `bootDFUComPort: "cu.usbmodem2072309B42311"` | The bootloader appears as a USB serial port (the MIDI ports vanish). The port name carries the chip's serial number |
| 3 | `STUARTDFU_init`, `STUARTDFU_getID` | Handshake and chip-ID query (`STM32UARTUpdater`) |
| 4 | `STUARTDFU_eraseQSPI` ×2, `skip QSPI` | No external flash on this model (`needQSPI false`) |
| 5 | `writeInt(1) 134250496 : N`, 3 phases per chunk | Write to `0x08008000 + N` in **32-byte chunks**: 3,072 chunks = 98,304 bytes. Phases look like command, address, data |
| 6 | `STUARTDFU_finished`, "Software update complete…" | The synth reboots itself into the application; MIDI ports return |

- The synth's button lights flash red during the write.
- Afterwards it reported 1.1.5 and answered a preset dump: the EEPROM contents survived.
- The bytes on the serial port were not captured (the tracer was on the wrong API). The exact framing can be read from `STM32UARTUpdater::writeInt`, `caculateChecksum`, `getDeviceID`.

How the run was set up, since SynthTribe refuses to update a synth that is already current:

- Proxyman rule 1 rewrites `version` in the `…/latest` reply to `1.1.6`.
- SynthTribe then asks `GET …/jt-4000-micro/1.1.6/60` for a download link (a 60-second signed link); rule 2 rewrites that path to `1.1.5`.
- The signed Aliyun link fails with `SignatureDoesNotMatch` when it passes through Proxyman, so rule 3 serves the identical file from disk.
- After repeated download failures SynthTribe's cloud login gets stuck ("up to date"); restarting it clears that.
- Tracing used an ad-hoc re-signed copy of SynthTribe run under lldb; the installed app is untouched.

## Update path: SynthTribe code (static analysis)

This table describes the DfuSe branch (`STM32USBUpdater`). The JT-4000 takes the serial branch (`STM32UARTUpdater::writeSetting`) instead; see above. Either way SynthTribe passes the encrypted image through unchanged.

| Step | Function | What happens to the bytes |
|---|---|---|
| 1 | `General_UpdateController::initial` | Reads `INTAddress 0x08008000`, `QSPIAddress`, `needQSPI false`, `bootAddress 0x2403FFFC` from `config.json` |
| 2 | `General_UpdateController::setFWData(QList<QVariant>)` | `QVariant::toByteArray` → `SettingItem`; no transform |
| 3 | `CUSBUpdateController::writeToMem(SettingItem&)` | Dispatches to `STM32USBUpdater::writeToMem` (USB) or `STM32UARTUpdater::writeSetting` (serial); `QByteArray` copy only |
| 4 | `STM32USBUpdater::updateDevice` | DfuSe loop: erase, set address (address split into bytes with `>>16`/`>>24`), download blocks. No XOR or rotate on the data |
| 5 | `STM32USBUpdater::download` | `libusb_control_transfer(bmRequestType 0x21, DFU_DNLOAD 1, wValue block, buffer)` |

There's no cipher code on this path: no XOR loops, no AES-NI, no key tables. The device's 32 KiB bootloader at `0x08000000` decrypts the image. The key turned out to be shared with Music Tribe's published packer tool; see Image format above.

Other paths in the same binary:

- `General_ProtocolZS::sendFirmwareData` sends prepared SysEx packets unchanged (`memcpy` → `sendMIDISysExMsg`). The packets come from `General_CFwSyx::setFWData`, which splits a `.syx` firmware file at `F0…F7` into ≤255-byte packets ("syx文件没有分包" = ".syx file not split into packets"). This path is for devices whose firmware ships as `.syx`.
- `STM32USBUpdater` also has `setReadProtection`, `eraseDevice` and `goCommand`, which are standard DfuSe operations.
- `CAppInstance::xorDecrypt(QString, QString)` is only called from `CAppInstance::initDeviceInfoFromDatabase` (device database), not from the firmware path.

### Ways to get at the key

- **USB capture of an update:** shows only the `.bin` we already have.
- **SWD (SWDIO/SWCLK test points):** if read-out protection (RDP) is level 0, the bootloader and key can be read directly. RDP level 1 blocks flash reads over the debugger.
- **Debug UART:** a logic analyzer could locate and decode one. The bootloader may log there.
