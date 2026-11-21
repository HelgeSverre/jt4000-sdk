"""Build a JT-4000 1.1.5 image whose "dump current preset" SysEx reads flash instead.

Request:  F0 00 20 32 00 01 38 00 00 <lo7> <hi7> F7     block = lo7 | hi7 << 7
Reply:    F0 00 20 32 00 01 38 15 <64 nibbles> 00 <cs> F7
          = 32 bytes at 0x08000000 + block * 32, high nibble first.

How: sysex_send_current_preset (0x08017064) calls eeprom_read(offset, buf, 0x40).
That one call is redirected to a 46-byte routine placed in the unused space after the
image's code. Nothing else changes; the routine only reads memory.

    uv run patches/flashdump.py        (needs arm-none-eabi-binutils)
"""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, "tools")
from fwpack import TRAILER, check, fix_trailer

BASE = 0x08008000
SRC = "firmware/decrypted/JT-4000_V115.bin"
OUT = "firmware/custom/flashdump-plain.bin"
FUNC, EEPROM_READ, RX_BUF = 0x08017064, 0x08016DC0, 0x20002ED0


def bl_target(code: bytes, addr: int) -> int | None:
    """Decode a Thumb-2 BL at addr; return its target or None."""
    h, l = struct.unpack("<2H", code)
    if h & 0xF800 != 0xF000 or l & 0xD000 != 0xD000:
        return None
    s, j1, j2 = (h >> 10) & 1, (l >> 13) & 1, (l >> 11) & 1
    i1, i2 = 1 - (j1 ^ s), 1 - (j2 ^ s)
    off = (s << 24) | (i1 << 23) | (i2 << 22) | ((h & 0x3FF) << 12) | ((l & 0x7FF) << 1)
    return addr + 4 + (off - (1 << 25) if s else off)


def bl_encode(addr: int, target: int) -> bytes:
    off = target - (addr + 4)
    s = (off >> 24) & 1
    i1, i2 = (off >> 23) & 1, (off >> 22) & 1
    j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
    h = 0xF000 | (s << 10) | ((off >> 12) & 0x3FF)
    l = 0xD000 | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7FF)
    return struct.pack("<2H", h, l)


img = bytearray(Path(SRC).read_bytes())
length = struct.unpack_from("<I", img, TRAILER)[0]
stub_addr = BASE + ((length + 3) & ~3)
assert not any(img[stub_addr - BASE : TRAILER]), "free space is not empty"

site = next(
    (
        a
        for a in range(FUNC, FUNC + 0x80, 2)
        if bl_target(img[a - BASE : a - BASE + 4], a) == EEPROM_READ
    ),
    None,
)
assert site, "call to eeprom_read not found"
assert (
    bl_target(bl_encode(site, EEPROM_READ), site) == EEPROM_READ
)  # encoder self-check
assert bl_encode(site, EEPROM_READ) == bytes(img[site - BASE : site - BASE + 4])

STUB = f"""
    .syntax unified
    .cpu cortex-m4
    .thumb
    push {{r4, r5, lr}}
    ldr  r3, rxbuf
    ldrb r4, [r3, #9]
    ldrb r5, [r3, #10]
    lsls r5, r5, #7
    orrs r4, r5
    lsls r4, r4, #5
    movs r5, #8
    lsls r5, r5, #24
    adds r4, r4, r5
    movs r3, #32
loop:
    ldrb r5, [r4]
    adds r4, #1
    lsrs r0, r5, #4
    strb r0, [r1]
    movs r0, #15
    ands r5, r0
    strb r5, [r1, #1]
    adds r1, #2
    subs r3, #1
    bne  loop
    movs r0, #0
    pop  {{r4, r5, pc}}
    .align 2
rxbuf:
    .word {RX_BUF:#x}
"""
with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    (d / "stub.s").write_text(STUB)
    subprocess.run(["arm-none-eabi-as", "-o", d / "stub.o", d / "stub.s"], check=True)
    subprocess.run(["arm-none-eabi-ld", "-Ttext", hex(stub_addr), "-o", d / "stub.elf", d / "stub.o"],
                   check=True, stderr=subprocess.DEVNULL)  # fmt: skip
    subprocess.run(
        ["arm-none-eabi-objcopy", "-O", "binary", d / "stub.elf", d / "stub.bin"],
        check=True,
    )
    stub = (d / "stub.bin").read_bytes()
    listing = subprocess.run(["arm-none-eabi-objdump", "-d", d / "stub.elf"], check=True,
                             capture_output=True, text=True).stdout  # fmt: skip
call = bl_encode(site, stub_addr)

img[stub_addr - BASE : stub_addr - BASE + len(stub)] = stub
img[site - BASE : site - BASE + 4] = call
struct.pack_into(
    "<I", img, TRAILER, stub_addr - BASE + len(stub)
)  # length covers the stub
fix_trailer(img)

print(f"call site {site:#x}: bl eeprom_read -> bl {stub_addr:#x}")
print(f"stub {len(stub)} bytes at {stub_addr:#x}:")
print("\n".join(l for l in listing.splitlines() if l.startswith(" ")))
check(img)
Path(OUT).write_bytes(img)
print("wrote", OUT)
