"""Package a plaintext JT-4000 application image into an update file.

Fixes the trailer (length, byte-sum, ACEFACEF marker) and encrypts with Speck-64/128 CBC.

    uv run tools/fwpack.py pack plain.bin out_CRY.bin
    uv run tools/fwpack.py check plain.bin
    uv run tools/fwpack.py build raw.bin out_CRY.bin     # raw linker output of any size
    uv run tools/fwpack.py patch-string in.bin out.bin "JT-4000 MICRO     " "JT-4001 MICRO     "
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys

import cli
from fwcrypt import cbc_decrypt, cbc_encrypt

KEY = b"-MakeLoveNotWar-"
SIZE = 0x18000
TRAILER = SIZE - 16
MARKER = bytes.fromhex("acefacef")


def trailer(img: bytes) -> tuple[int, int, bytes]:
    length, cksum = struct.unpack_from("<2I", img, TRAILER)
    return length, cksum, img[TRAILER + 8 : TRAILER + 12]


def check(img: bytes) -> bool:
    length, cksum, marker = trailer(img)
    ok = len(img) == SIZE and marker == MARKER and cksum == sum(img[: length + 1])
    print(f"size {len(img):#x} length {length:#x} checksum {cksum:#010x} "
          f"(computed {sum(img[: length + 1]):#010x}) marker {marker.hex()} -> {'OK' if ok else 'BAD'}")  # fmt: skip
    return ok


def fix_trailer(img: bytearray) -> None:
    length = struct.unpack_from("<I", img, TRAILER)[0]
    struct.pack_into("<I", img, TRAILER + 4, sum(img[: length + 1]))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("plain")
    k = sub.add_parser("pack")
    k.add_argument("plain")
    k.add_argument("out")
    s = sub.add_parser(
        "patch-string", help="replace one same-length ASCII string, fix trailer"
    )
    s.add_argument("plain")
    s.add_argument("out")
    s.add_argument("old")
    s.add_argument("new")
    b = sub.add_parser("build", help="pad a raw image, write the trailer, encrypt")
    b.add_argument("plain")
    b.add_argument("out")
    a = p.parse_args()
    with open(a.plain, "rb") as f:
        img = bytearray(f.read())
    if a.cmd == "build":
        if len(img) > TRAILER:
            sys.exit(
                f"error: {a.plain} is {len(img)} bytes; the application area holds {TRAILER}. "
                "The firmware is too big."
            )
        if len(img) < 8:
            sys.exit(f"error: {a.plain} is {len(img)} bytes: not a firmware image")
        code = len(img)
        img += bytes(SIZE - len(img))
        # The bootloader sums bytes 0..length inclusive and needs 0x10000 <= length.
        struct.pack_into("<I", img, TRAILER, max((code + 3) & ~3, 0x10000))
        img[TRAILER + 8 : TRAILER + 12] = MARKER
        print(f"code {code} bytes ({code * 100 // TRAILER}% of flash)")
    elif len(img) != SIZE:
        sys.exit(
            f"error: {a.plain} is {len(img)} bytes; '{a.cmd}' needs a complete {SIZE}-byte "
            "plain image. Use 'build' for raw linker output."
        )
    if a.cmd == "check":
        sys.exit(0 if check(img) else 1)
    if a.cmd == "patch-string":
        old, new = a.old.encode(), a.new.encode()
        if len(old) != len(new) or img.count(old) != 1:
            sys.exit(
                f"error: the two strings must be the same length ({len(old)} and {len(new)}) "
                f"and the old one must occur exactly once (found {img.count(old)})"
            )
        at = img.index(old)
        img[at : at + len(old)] = new
        fix_trailer(img)
        print(f"patched at image offset {at:#x} (address {0x08008000 + at:#x})")
        check(img)
        with open(a.out, "wb") as f:
            f.write(img)
        return
    fix_trailer(img)
    if not check(img):
        sys.exit("error: refusing to pack an image the bootloader would reject")
    enc = cbc_encrypt(KEY, bytes(img))
    assert cbc_decrypt(KEY, enc) == bytes(img)
    with open(a.out, "wb") as f:
        f.write(enc)
    print(f"wrote {a.out}: {len(enc)} bytes, md5 {hashlib.md5(enc).hexdigest()}")


if __name__ == "__main__":
    cli.run(main)
