"""Flash a firmware image to the JT-4000 without SynthTribe.

Speaks the bootloader's serial protocol (ST AN3155 style, read from the bootloader's
handler at 0x08003C40):

    7F                       -> 79                sync
    02 FD                    -> 79 01 04 21 79    get ID
    AA 55                    -> 79 0E <14 bytes> 79   bootloader info
    44 BB, then FF FF 00     -> 79, 79            erase the application area
    31 CE, addr+xor, N-1 data xor -> 79 79 79     write (bootloader decrypts, then programs)
    21 DE                    -> 79                start the application

The bootloader's CBC state is set once at start-up, so the image must be written once,
in order, from 0x08008000, in multiples of 8 bytes.

    uv run tools/flash.py info
    uv run tools/flash.py flash firmware/JT-4000_INTFLASH_CRY_V115.bin
    uv run tools/flash.py flash firmware/custom/some-plain.bin       # plain images are packed first
"""

from __future__ import annotations

import argparse
import sys
import time

import mido
import serial
from fwcrypt import cbc_decrypt, cbc_encrypt
from fwpack import KEY, MARKER, SIZE, TRAILER, trailer
from serial.tools import list_ports
from sysex import HEADER, PORT_HINTS

ACK, NACK = 0x79, 0x1F
BASE = 0x08008000
CHUNK = 32
VID, PID = 0x1397, 0x127E  # Behringer, "JT-4000 MICRO BOOTLOADER"
CHIP_ID = bytes([0x01, 0x04, 0x21])
INFO_PREFIX = (
    b"BE_P0EJW_BOOT"  # the JT-4000 Micro's bootloader; a version digit follows
)


def find_serial() -> str | None:
    """The JT-4000 bootloader's serial port. Other Behringer devices are ignored."""
    found = [p.device for p in list_ports.comports() if (p.vid, p.pid) == (VID, PID)]
    if len(found) > 1:
        sys.exit(
            f"{len(found)} JT-4000 bootloaders are connected ({', '.join(found)}). "
            "Leave only the one to flash plugged in."
        )
    return found[0] if found else None


def midi_out_name() -> str | None:
    return next(
        (n for n in mido.get_output_names() if any(h in n for h in PORT_HINTS)), None
    )


def enter_update_mode(wait: float = 15.0) -> str:
    dev = find_serial()
    if dev:
        return dev
    name = midi_out_name()
    if not name:
        sys.exit(
            "synth not found. Power it on, or hold OSC + SETTINGS while powering on."
        )
    print(f"sending 'reboot to update mode' to {name}")
    with mido.open_output(name) as out:
        out.send(mido.Message("sysex", data=[*HEADER, 0x02]))
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        dev = find_serial()
        if dev:
            time.sleep(0.5)
            return dev
        time.sleep(0.2)
    sys.exit("the bootloader's serial port did not appear")


class Boot:
    def __init__(self, dev: str):
        self.s = serial.Serial(dev, 115200, timeout=3)

    def ack(self, what: str, timeout: float | None = None) -> None:
        if timeout:
            self.s.timeout = timeout
        b = self.s.read(1)
        self.s.timeout = 3
        if b != bytes([ACK]):
            raise RuntimeError(f"{what}: expected ACK, got {b.hex() or 'nothing'}")

    def cmd(self, c: int, what: str) -> None:
        self.s.write(bytes([c, c ^ 0xFF]))
        self.ack(what)

    def sync(self) -> None:
        self.s.reset_input_buffer()
        self.s.write(b"\x7f")
        self.ack("sync")

    def get_id(self) -> bytes:
        self.cmd(0x02, "get id")
        d = self.s.read(3)
        self.ack("get id end")
        return d

    def info(self) -> bytes:
        self.cmd(0xAA, "info")
        n = self.s.read(1)[0]
        d = self.s.read(n)
        self.ack("info end")
        return d

    def erase(self) -> None:
        self.cmd(0x44, "erase")
        self.s.write(b"\xff\xff\x00")
        self.ack("erase done", timeout=20)

    def write(self, addr: int, data: bytes) -> None:
        self.cmd(0x31, "write")
        a = addr.to_bytes(4, "big")
        self.s.write(a + bytes([a[0] ^ a[1] ^ a[2] ^ a[3]]))
        self.ack("write address")
        x = len(data) - 1
        for b in data:
            x ^= b
        self.s.write(bytes([len(data) - 1]) + data + bytes([x]))
        self.ack("write data")

    def identify(self) -> tuple[bytes, bytes]:
        """Sync and return (chip id, info), or stop if this is not a JT-4000 bootloader.
        Nothing on the device has been changed when this refuses."""
        try:
            self.sync()
            chip, info = self.get_id(), self.info()
        except (serial.SerialException, RuntimeError, OSError) as e:
            sys.exit(
                f"the bootloader on {self.s.port} does not answer ({e}). Switch the synth "
                "off and on, then try again. Nothing was erased."
            )
        if chip != CHIP_ID or not info.startswith(INFO_PREFIX):
            sys.exit(
                f"refusing: {self.s.port} does not answer like a JT-4000 bootloader "
                f"(chip id {chip.hex(' ')}, info {info!r}). Nothing was erased."
            )
        return chip, info

    def go(self) -> None:
        """Start the application. The serial port usually vanishes before the ACK
        arrives, so silence or a dropped port is fine; any other reply is a refusal."""
        try:
            self.s.write(b"\x21\xde")
            self.s.timeout = 1
            reply = self.s.read(1)
        except (serial.SerialException, OSError):
            return
        if reply and reply != bytes([ACK]):
            what = "NACK" if reply == bytes([NACK]) else reply.hex()
            sys.exit(f"the bootloader refused to start the application ({what})")


def wait_for_application(boot: Boot, wait: float = 8.0) -> None:
    """Confirm the bootloader has gone, then look for the synth's MIDI port."""
    deadline = time.monotonic() + wait
    while find_serial():
        if time.monotonic() > deadline:
            # The stock firmware, started this way, leaves the bootloader's USB device
            # listed on the host. A bootloader that is really still there answers.
            try:
                boot.get_id()
            except (serial.SerialException, RuntimeError, OSError):
                print(
                    "started the application: the bootloader no longer answers. Its USB "
                    "port is still listed, so switch the synth off and on to get USB MIDI back."
                )
                return
            sys.exit(
                "the application did not start: the synth is still in update mode. "
                "The image is written; power-cycle the synth, or flash again."
            )
        time.sleep(0.2)
    while time.monotonic() < deadline:
        name = midi_out_name()
        if name:
            print(f"started the application: the synth is back as MIDI port '{name}'")
            return
        time.sleep(0.3)
    print(
        "started the application: the bootloader has gone. No MIDI port appeared, "
        "which is normal for firmware that does not call jt_usb_init()."
    )


def load(path: str) -> bytes:
    """Return the encrypted image, packing a plain one if needed. Refuse bad trailers."""
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) != SIZE:
        sys.exit(f"{path}: expected {SIZE} bytes, got {len(raw)}")
    plain, enc = cbc_decrypt(KEY, raw), raw
    if trailer(plain)[2] != MARKER:
        plain, enc = raw, cbc_encrypt(KEY, raw)
    length, cksum, marker = trailer(plain)
    ok = (
        marker == MARKER
        and 0x10000 <= length <= TRAILER
        and cksum == sum(plain[: length + 1])
    )
    print(f"{path}: length {length:#x}, checksum {cksum:#010x}, marker {marker.hex()}")
    if not ok:
        sys.exit(
            "refusing: the bootloader would reject this image (marker, length or checksum)"
        )
    sp, reset = (
        int.from_bytes(plain[0:4], "little"),
        int.from_bytes(plain[4:8], "little"),
    )
    if not 0x20000008 <= sp <= 0x20007FF8:
        sys.exit(f"refusing: initial SP {sp:#x}; the bootloader pops 8 bytes from it before "
                 "jumping, so it must be at most 0x20007FF8")  # fmt: skip
    if not (0x08008000 <= reset < 0x08020000 and reset & 1):
        sys.exit(
            f"refusing: reset vector {reset:#x} is not a Thumb address in the application area"
        )
    if any(plain[length + 1 : TRAILER]):
        sys.exit(
            "refusing: non-zero bytes after 'length' are not covered by the checksum"
        )
    return enc


def cmd_info(_a) -> None:
    b = Boot(enter_update_mode())
    chip, info = b.identify()
    print("chip id:", chip.hex(" "))
    print("bootloader info:", info.decode("ascii", "replace"))
    print("left in update mode; power-cycle to return to the application")


def cmd_flash(a) -> None:
    enc = load(a.file)
    b = Boot(enter_update_mode())
    chip, info = b.identify()
    print(f"bootloader: {info.decode('ascii', 'replace')}, chip id {chip.hex(' ')}")
    print("erasing application area ...")
    b.erase()
    t0 = time.monotonic()
    for off in range(0, SIZE, CHUNK):
        b.write(BASE + off, enc[off : off + CHUNK])
        if off % 4096 == 0:
            print(f"\rwriting {off:#7x} / {SIZE:#x}", end="", flush=True)
    print(f"\rwrote {SIZE:#x} bytes in {time.monotonic() - t0:.1f} s")
    b.go()
    wait_for_application(b)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser(
        "info", help="enter update mode and print bootloader details"
    ).set_defaults(fn=cmd_info)
    f = sub.add_parser("flash", help="erase, write and start an image")
    f.add_argument("file")
    f.set_defaults(fn=cmd_flash)
    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
