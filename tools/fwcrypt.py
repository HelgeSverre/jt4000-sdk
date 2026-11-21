"""Speck-64/128 CBC as used by Music Tribe's PRO800_update_gen, plus key testing.

Recovered from MusicTribeOpenSource/PRO800-Firmware-Packer (linux/PRO800_update_gen):
  - key schedule fcn.00001980: 4 little-endian key words, 27 rounds
  - CBC encrypt   fcn.00001ac0: block = (y, x) little-endian words, IV 11223344 55667788
  - header        fcn.00001b40: plaintext[0x18:0x1C] = tag ("P800"),
                                [0x20:0x24] = length, [0x24:0x28] = CRC-32/MPEG-2

    uv run tools/fwcrypt.py selftest
    uv run tools/fwcrypt.py try firmware/JT-4000_INTFLASH_CRY_V115.bin --key <32 hex>
    uv run tools/fwcrypt.py scan firmware/*.bin        # try guessable keys
"""

from __future__ import annotations

import argparse
import struct
import sys

M = 0xFFFFFFFF
ROUNDS = 27
IV = (0x11223344, 0x55667788)  # (y, x) as loaded from .rodata 0x2048


def ror(v: int, n: int) -> int:
    return ((v >> n) | (v << (32 - n))) & M


def rol(v: int, n: int) -> int:
    return ((v << n) | (v >> (32 - n))) & M


def expand(key: bytes) -> list[int]:
    k, *ls = struct.unpack("<4I", key)
    rks = []
    for i in range(ROUNDS):
        rks.append(k)
        j = i % 3
        ls[j] = ((ror(ls[j], 8) + k) & M) ^ i
        k = rol(k, 3) ^ ls[j]
    return rks


def enc_block(rks: list[int], y: int, x: int) -> tuple[int, int]:
    for rk in rks:
        x = ((ror(x, 8) + y) & M) ^ rk
        y = rol(y, 3) ^ x
    return y, x


def dec_block(rks: list[int], y: int, x: int) -> tuple[int, int]:
    for rk in reversed(rks):
        y = ror(y ^ x, 3)
        x = rol(((x ^ rk) - y) & M, 8)
    return y, x


def cbc_encrypt(key: bytes, data: bytes, iv=IV) -> bytes:
    rks, (py, px), out = expand(key), iv, bytearray()
    for y, x in struct.iter_unpack("<2I", data):
        py, px = enc_block(rks, y ^ py, x ^ px)
        out += struct.pack("<2I", py, px)
    return bytes(out)


def cbc_decrypt(key: bytes, data: bytes, iv=IV) -> bytes:
    rks, (py, px), out = expand(key), iv, bytearray()
    for y, x in struct.iter_unpack("<2I", data):
        dy, dx = dec_block(rks, y, x)
        out += struct.pack("<2I", dy ^ py, dx ^ px)
        py, px = y, x
    return bytes(out)


def score(plain: bytes) -> int:
    """Count Cortex-M-looking vectors (0x0800xxxx/0x0801xxxx, thumb bit set).

    Words 2.. come from blocks 1.., whose CBC decryption doesn't depend on the IV.
    """
    words = struct.unpack_from("<14I", plain, 8)
    return sum(1 for w in words if w >> 17 == 0x0800_0000 >> 17 and w & 1)


def candidates() -> dict[str, bytes]:
    c: dict[str, bytes] = {}

    def add(name: str, raw: bytes) -> None:
        c[name] = (raw + bytes(16))[:16]
        c[name + " (repeat)"] = (raw * 16)[:16]

    for s in (
        "P0EJW", "JT4K", "JT-4000", "JT-4000 MICRO", "JT4000", "SATURN-4K", "BEHRINGER",
        "BEHRINGER JT-4000", "BEHRINGER JT-4000 MICRO", "behringer", "musictribe",
        "MUSICTRIBE", "Music Tribe", "SYNTHTRIBE", "P800", "bySG", "P800bySG", "0722-ABR",
        "jt-4000-micro", "INTFLASH", "CRY",
    ):  # fmt: skip
        add(repr(s), s.encode())
    add("zeros", b"\0")
    add("ff", b"\xff")
    add("00..0f", bytes(range(16)))
    add("01..10", bytes(range(1, 17)))
    add("iv bytes", struct.pack("<2I", *IV))
    add(
        "11223344 55667788 99aabbcc ddeeff00",
        bytes.fromhex("112233445566778899aabbccddeeff00"),
    )
    add(
        "LE words 11223344..",
        struct.pack("<4I", 0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00),
    )
    add("speck test key", bytes.fromhex("0001020308090a0b1011121318191a1b"))
    add(
        "0123456789abcdef fedcba9876543210",
        bytes.fromhex("0123456789abcdeffedcba9876543210"),
    )
    return c


def cmd_selftest(_a) -> None:
    # Speck64/128 vector from the Speck paper: key 1b1a1918 13121110 0b0a0908 03020100,
    # pt 3b726574 7475432d, ct 8c6fa548 454e028b (x, y word order).
    key = struct.pack("<4I", 0x03020100, 0x0B0A0908, 0x13121110, 0x1B1A1918)
    y, x = enc_block(expand(key), 0x7475432D, 0x3B726574)
    assert (x, y) == (0x8C6FA548, 0x454E028B), f"{x:08x} {y:08x}"
    assert dec_block(expand(key), y, x) == (0x7475432D, 0x3B726574)
    data = bytes(range(64))
    assert cbc_decrypt(key, cbc_encrypt(key, data)) == data
    print("selftest ok: matches the published Speck64/128 test vector")


def show(plain: bytes) -> None:
    for i in range(0, 48, 16):
        print(f"  {i:04x}: {plain[i : i + 16].hex(' ')}")
    print(f"  score {score(plain)}/14, tag {plain[0x18:0x1C]!r}")


def cmd_try(a) -> None:
    with open(a.file, "rb") as f:
        data = f.read()
    plain = cbc_decrypt(bytes.fromhex(a.key), data)
    show(plain)
    if a.out:
        with open(a.out, "wb") as f:
            f.write(plain)
        print(f"wrote {a.out}")


def cmd_scan(a) -> None:
    cands = candidates()
    for path in a.files:
        with open(path, "rb") as f:
            head = f.read(128)
        best = max(((score(cbc_decrypt(k, head)), n) for n, k in cands.items()))
        print(f"{path}: {len(cands)} keys tried, best score {best[0]}/14 ({best[1]})")
        if best[0] >= 6:
            show(cbc_decrypt(cands[best[1]], head))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("selftest").set_defaults(fn=cmd_selftest)
    t = sub.add_parser("try", help="decrypt with a given key and show the header")
    t.add_argument("file")
    t.add_argument("--key", required=True, help="16 bytes as hex")
    t.add_argument("-o", "--out")
    t.set_defaults(fn=cmd_try)
    s = sub.add_parser("scan", help="try a list of guessable keys")
    s.add_argument("files", nargs="+")
    s.set_defaults(fn=cmd_scan)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
