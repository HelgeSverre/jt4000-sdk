"""Read MCU flash from a JT-4000 running the FLASHDUMP image (patches/flashdump.py).

uv run tools/flashdump.py verify                      # compare app area with the known image
uv run tools/flashdump.py read 0x08000000 0x8000 -o firmware/bootloader.bin
"""

from __future__ import annotations

import argparse
import sys
import time

import mido
from sysex import HEADER, checksum, open_ports

FLASH = 0x08000000
BLOCK = 32


def read_block(out, inp, block: int, tries: int = 4) -> bytes:
    req = mido.Message(
        "sysex", data=[*HEADER, 0x00, 0x00, block & 0x7F, (block >> 7) & 0x7F]
    )
    for _ in range(tries):
        for _ in inp.iter_pending():
            pass
        out.send(req)
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            for m in inp.iter_pending():
                d = list(m.data) if m.type == "sysex" else []
                if len(d) == 73 and tuple(d[:6]) == HEADER and d[6] == 0x15:
                    nib = d[7:71]
                    if max(nib) < 16 and checksum(nib) == d[72]:
                        return bytes(nib[i] << 4 | nib[i + 1] for i in range(0, 64, 2))
            time.sleep(0.001)
    sys.exit(f"no valid reply for block {block:#x}")


def read(out, inp, addr: int, size: int, progress: bool = True) -> bytes:
    assert addr % BLOCK == 0 and size % BLOCK == 0
    buf = bytearray()
    first = (addr - FLASH) // BLOCK
    for i in range(size // BLOCK):
        buf += read_block(out, inp, first + i)
        if progress and i % 64 == 0:
            print(f"\r{len(buf):#7x} / {size:#x}", end="", flush=True)
    if progress:
        print(f"\r{len(buf):#7x} / {size:#x}")
    return bytes(buf)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("verify")
    r = sub.add_parser("read")
    r.add_argument("addr", type=lambda s: int(s, 0))
    r.add_argument("size", type=lambda s: int(s, 0))
    r.add_argument("-o", "--out", required=True)
    a = p.parse_args()
    out, inp, _ = open_ports(a.port)
    with out, inp:
        if a.cmd == "verify":
            with open("firmware/custom/flashdump-plain.bin", "rb") as f:
                want = f.read()
            got = read(out, inp, 0x08008000, 0x400, progress=False)
            ok = got == want[:0x400]
            print("app area 0x08008000..0x080083FF matches the flashed image:", ok)
            sys.exit(0 if ok else 1)
        data = read(out, inp, a.addr, a.size)
        with open(a.out, "wb") as f:
            f.write(data)
        print(f"wrote {a.out} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
