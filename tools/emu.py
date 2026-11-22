"""Run a JT-4000 application image on the host and show what it draws.

A minimal model of the board, enough for firmware bring-up:
  - Cortex-M4 core (Unicorn), flash at 0x08008000, 32 KiB RAM
  - RCC ready flags, SysTick COUNTFLAG (each poll advances time by 1 ms)
  - GPIOA/GPIOB, with the OLED's bit-banged SPI decoded into an SSD1306 model
  - OSC (PB4) and SETTINGS (PA10) buttons

    uv run --with unicorn tools/emu.py sdk/build/hello/hello.bin
    uv run --with unicorn tools/emu.py sdk/build/hello/hello.bin --ms 500 --osc --settings
"""

from __future__ import annotations

import argparse
import itertools
import struct
import sys
from typing import ClassVar

import cli

try:
    import unicorn  # noqa: F401
except ModuleNotFoundError:
    sys.exit(
        "error: needs the 'unicorn' module. Run: uv run --with unicorn tools/emu.py ..."
    )

from unicorn import (
    UC_ARCH_ARM,
    UC_HOOK_BLOCK,
    UC_HOOK_MEM_READ,
    UC_HOOK_MEM_WRITE,
    UC_MODE_MCLASS,
    UC_MODE_THUMB,
    Uc,
    UcError,
)
from unicorn.arm_const import (
    UC_ARM_REG_C1_C0_2,
    UC_ARM_REG_FPEXC,
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_SP,
)

FLASH, RAM = 0x08008000, 0x20000000
RCC, GPIOA, GPIOB = 0x40021000, 0x48000000, 0x48000400
ADC1 = 0x50000000
SYST_CSR, AIRCR = 0xE000E010, 0xE000ED0C


class Oled:
    """SSD1306, 128 columns x 8 pages. Tracks page/column addressing and display RAM."""

    ARGS = {  # noqa: RUF012
        0x81: 1,
        0xA8: 1,
        0xD3: 1,
        0xD5: 1,
        0xD9: 1,
        0xDA: 1,
        0xDB: 1,
        0x8D: 1,
        0x20: 1,
    }

    def __init__(self):
        self.ram = bytearray(128 * 8)
        self.page = self.col = self.skip = 0
        self.on = False
        self.cmds: list[int] = []
        self.data_bytes = 0

    def command(self, c: int) -> None:
        self.cmds.append(c)
        if self.skip:
            self.skip -= 1
        elif c in self.ARGS:
            self.skip = self.ARGS[c]
        elif 0xB0 <= c <= 0xB7:
            self.page = c & 7
        elif c <= 0x0F:
            self.col = (self.col & 0xF0) | c
        elif 0x10 <= c <= 0x1F:
            self.col = (self.col & 0x0F) | ((c & 0x0F) << 4)
        elif c in (0xAE, 0xAF):
            self.on = c == 0xAF

    def data(self, b: int) -> None:
        self.data_bytes += 1
        if self.col < 128:
            self.ram[self.page * 128 + self.col] = b
            self.col += 1

    def render(self, rows: int = 32) -> str:
        out = []
        for y in range(0, rows, 2):
            line = ""
            for x in range(128):
                top = (self.ram[(y >> 3) * 128 + x] >> (y & 7)) & 1
                bot = (self.ram[((y + 1) >> 3) * 128 + x] >> ((y + 1) & 7)) & 1
                line += " ▀▄█"[top | bot << 1]
            out.append(line.rstrip())
        return "\n".join(out)


class Board:
    def __init__(self, image: bytes, osc: bool, settings: bool):
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        uc.mem_map(0x08000000, 0x20000)
        uc.mem_map(RAM, 0x8000)
        uc.mem_map(0x40000000, 0x30000)
        uc.mem_map(0x48000000, 0x2000)
        uc.mem_map(0x50000000, 0x1000)  # ADC1/2
        uc.mem_map(0xE0000000, 0x100000)
        uc.mem_write(FLASH, image)
        uc.mem_write(RCC, struct.pack("<I", 0x00000500))  # HSI on and ready
        uc.mem_write(
            RCC + 8, struct.pack("<I", 0x0000000F)
        )  # pretend the PLL is the clock
        self.oled = Oled()
        self.odr = {GPIOA: 0, GPIOB: 0}
        self.idr = {
            GPIOA: 0xFFFF & ~(0x400 if settings else 0),
            GPIOB: 0xFFFF & ~(0x10 if osc else 0),
        }
        self.shift = self.bits = 0
        self.i2c: list[int] = []
        self.i2c_bits: list[int] = []
        self.ms = 0
        # ADC channel -> reading: cutoff open, no resonance, keyboard line idle
        self.pots = {3: 4095, 4: 0, 15: 4095}
        self.dac: list[int] = []
        self.blocks = 0
        self.nvic = 0  # enabled interrupts 0..31
        self.audio_ms = 0  # simulated time covered by delivered audio blocks
        self.kb = 0  # touch-keyboard bits returned on I2C bus 2
        self.kb_bit = -1
        self.keys = 0  # scanned buttons, as the key chip's first byte (JT_KEY_* bits)
        self.key_bit = -1
        self.key_read = False
        self.enc = 0  # SELECT knob count (TIM2)
        self.adc_dma: tuple[int, int] | None = None  # ADC sequence register, DMA target
        self.events: list[tuple[int, str]] = []  # (ms, action), sorted
        self.shots: list[tuple[int, str]] = []
        self.pwm: dict[str, int] = {}
        self.leds: tuple[int, int] | None = None
        self.reset = False
        self.log: list[str] = []
        uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write, begin=0x40000000, end=0xE00FFFFF)
        uc.hook_add(UC_HOOK_MEM_READ, self.on_read, begin=0x40000000, end=0xE00FFFFF)
        uc.hook_add(UC_HOOK_BLOCK, self.on_block)
        self.irq_bytes = None  # code bytes executed by the audio interrupt being timed
        self.irq_worst = 0
        uc.reg_write(
            UC_ARM_REG_C1_C0_2, 0xF << 20
        )  # FPU usable: the SDK builds hard-float
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        sp, pc = struct.unpack_from("<2I", image)
        # The bootloader sets SP = vector[0] and pops 8 bytes before jumping.
        if sp + 8 > RAM + 0x8000:
            sys.exit(
                f"initial SP {sp:#x} is too high: the bootloader's pop would read past the end of RAM"
            )
        uc.reg_write(UC_ARM_REG_SP, sp + 8)
        self.entry = pc

    def w32(self, addr: int, v: int) -> None:
        self.uc.mem_write(addr, struct.pack("<I", v & 0xFFFFFFFF))

    def r32(self, addr: int) -> int:
        return struct.unpack("<I", self.uc.mem_read(addr, 4))[0]

    def on_read(self, uc, access, addr, size, value, user):
        if addr == RCC:  # ready flags follow the enable bits
            cr = self.r32(RCC)
            cr = (
                (cr & ~0x02020400)
                | ((cr & 0x100) << 2)
                | ((cr & 0x10000) << 1)
                | ((cr & 0x1000000) << 1)
            )
            self.w32(RCC, cr)
        elif addr == RCC + 8:  # SWS follows SW
            cfgr = self.r32(RCC + 8)
            self.w32(RCC + 8, (cfgr & ~0xC) | ((cfgr & 3) << 2))
        elif addr in (GPIOA + 0x10, GPIOB + 0x10):
            self.w32(addr, self.idr[addr - 0x10])
        elif addr == ADC1 + 8:  # CR: calibration and start bits clear themselves
            self.w32(addr, self.r32(addr) & ~0x80000004)
        elif addr == ADC1 + 0x40:  # DR: the selected channel's pot
            self.w32(addr, self.pots.get(self.r32(ADC1 + 0x30) >> 6 & 31, 0))
        elif addr == 0x40001010:  # TIM6 SR: one audio sample tick per poll
            self.w32(addr, 1)
            if len(self.dac) % 32 == 31:
                self.ms += 1
        elif addr == 0x40000024:  # TIM2 CNT: the SELECT knob
            self.w32(addr, self.enc)
        elif addr == SYST_CSR:
            csr = self.r32(SYST_CSR)
            if csr & 1:
                self.ms += 1
                self.w32(SYST_CSR, csr | 0x10000)

    def on_write(self, uc, access, addr, size, value, user):
        base = addr & ~0x3FF
        if addr == 0xE000E100:  # NVIC ISER0: writing 1 sets, 0 leaves alone
            self.nvic |= value
        elif addr == 0xE000E180:  # ICER0
            self.nvic &= ~value
        if base in self.odr and (addr & 0x3FF) == 0x18:  # BSRR
            old = self.odr[base]
            new = (old | (value & 0xFFFF)) & ~(value >> 16)
            self.odr[base] = new
            if base == GPIOB and not old & 8 and new & 8 and not self.odr[GPIOA] & 0x40:
                self.shift = (
                    self.shift << 1 | (self.odr[GPIOA] >> 7 & 1)
                ) & 0xFF  # SCK rise, CS low
                self.bits += 1
                if self.bits == 8:
                    self.bits = 0
                    (self.oled.data if self.odr[GPIOA] & 0x8000 else self.oled.command)(
                        self.shift
                    )
            if base == GPIOB and (old ^ new) & 0x180:  # I2C on PB7 (SDA) / PB8 (SCL)
                sda_o, scl_o, sda_n, scl_n = (
                    old >> 7 & 1,
                    old >> 8 & 1,
                    new >> 7 & 1,
                    new >> 8 & 1,
                )
                if scl_o and scl_n and sda_o and not sda_n:
                    self.key_bit, self.key_read = -1, False  # START
                elif scl_o and not scl_n and self.key_bit < 80:
                    # The LED/key chip: acknowledge every byte; on a read, answer with the
                    # key-scan bytes (the first holds the buttons, the rest are zero).
                    self.key_bit += 1
                    n, out = self.key_bit, 1
                    if n == 8:
                        bits = self.i2c_bits[:8]
                        self.key_read = len(bits) == 8 and bits[7] == 1
                        out = 0
                    elif self.key_read and n > 8 and (n - 9) % 9 < 8:
                        byte = self.keys if n < 18 else 0
                        out = byte >> (7 - (n - 9) % 9) & 1
                    elif not self.key_read and n % 9 == 8:
                        out = 0
                    self.idr[GPIOB] = (self.idr[GPIOB] & ~0x80) | out << 7
                if scl_o and scl_n and sda_o != sda_n:
                    if sda_n and len(self.i2c) == 18 and self.i2c[:2] == [0xE0, 0x00]:
                        leds = (
                            self.i2c[6],
                            self.i2c[8],
                        )  # LED chip display rows 4 and 6
                        if leds != self.leds:
                            self.leds = leds
                            self.log.append(
                                f"{self.ms:5d} ms  LEDs row4={leds[0]:06b} row6={leds[1]:08b}"
                            )
                    elif sda_n and self.i2c and len(self.log) < 60:
                        self.log.append(
                            f"{self.ms:5d} ms  I2C {' '.join(f'{b:02X}' for b in self.i2c)}"
                        )
                    self.i2c, self.i2c_bits = [], []
                elif scl_n and not scl_o:
                    self.i2c_bits.append(sda_n)
                    if len(self.i2c_bits) == 9:
                        self.i2c.append(int("".join(map(str, self.i2c_bits[:8])), 2))
                        self.i2c_bits = []
            if (
                base == GPIOA and (old ^ new) & 0x300
            ):  # I2C bus 2 on PA8 (SDA) / PA9 (SCL)
                sda_o, scl_o, sda_n, scl_n = (
                    old >> 8 & 1,
                    old >> 9 & 1,
                    new >> 8 & 1,
                    new >> 9 & 1,
                )
                if scl_o and scl_n and sda_o and not sda_n:
                    self.kb_bit = -1  # START: the next falling clock edge begins bit 0
                elif scl_o and not scl_n and self.kb_bit < 40:
                    self.kb_bit += 1
                    n, out = (
                        self.kb_bit,
                        1,
                    )  # the keyboard chip answers a read with 2 bytes
                    if n == 8:
                        out = 0  # acknowledge the address
                    elif 9 <= n <= 16:
                        out = (self.kb & 0xFF) >> (16 - n) & 1
                    elif 18 <= n <= 25:
                        out = (self.kb >> 8) >> (25 - n) & 1
                    self.idr[GPIOA] = (self.idr[GPIOA] & ~0x100) | out << 8
            if base == GPIOA and (old ^ new) & 0x20:
                self.log.append(
                    f"{self.ms:5d} ms  OLED reset {'released' if new & 0x20 else 'asserted'}"
                )
            if base == GPIOA and not old & 0x40 and new & 0x40:
                self.bits = 0  # chip select released
        elif addr == 0x5000080C:  # DAC DHR12L1
            self.dac.append(value & 0xFFFF)
        elif addr in (0x4001343C, 0x40000834):  # filter PWM compare values
            self.pwm["cutoff" if addr == 0x4001343C else "resonance(inv)"] = value
        elif addr == ADC1 + 8:  # CR: calibration ends at once
            uc.mem_write(
                ADC1,
                struct.pack(
                    "<I",
                    self.r32(ADC1) | (1 if value & 1 else 0) | (4 if value & 4 else 0),
                ),
            )
        if addr == ADC1 + 8 and value & 4 and self.r32(ADC1 + 0xC) & 1:
            sqr, dst = (
                self.r32(ADC1 + 0x30),
                self.r32(0x40020028),
            )  # sequence, DMA1 ch2 target
            self.adc_dma = (sqr, dst)
            self.adc_refresh()
        if addr == AIRCR and value & 4:
            self.reset = True
            self.log.append(f"{self.ms:5d} ms  system reset requested")
            uc.emu_stop()

    def adc_refresh(self) -> None:
        """Put the knob readings where the firmware's ADC DMA transfer leaves them."""
        if not self.adc_dma:
            return
        sqr, dst = self.adc_dma
        for i in range((sqr & 15) + 1):
            ch = sqr >> (6 + 6 * i) & 31
            self.uc.mem_write(dst + 2 * i, struct.pack("<H", self.pots.get(ch, 0)))

    def on_block(self, uc, addr, size, user):
        if self.irq_bytes is not None:
            self.irq_bytes += size

    def audio_irq(self) -> bool:
        """Deliver one DMA half/full interrupt if the firmware has set audio up."""
        if not (self.r32(0x40020008) & 1 and self.nvic & (1 << 11)):
            return False
        handler = struct.unpack("<I", self.uc.mem_read(FLASH + 4 * 27, 4))[0]
        ring, half = self.r32(0x40020014), self.r32(0x4002000C) // 2
        second = self.blocks & 1
        self.blocks += 1
        self.w32(0x40020000, 0x3 if second else 0x5)  # TCIF1 or HTIF1, plus GIF1
        ctx = self.uc.context_save()
        self.uc.reg_write(UC_ARM_REG_LR, 0x08000001)  # return lands in unused flash
        self.irq_bytes = 0
        self.uc.emu_start(handler | 1, 0x08000000, count=2_000_000)
        self.irq_worst = max(self.irq_worst, self.irq_bytes)
        self.irq_bytes = None
        self.uc.context_restore(ctx)
        data = self.uc.mem_read(ring + 2 * half * second, 2 * half)
        self.dac.extend(struct.unpack(f"<{half}H", data))
        self.audio_ms += 2
        return True

    BUTTONS: ClassVar[dict[str, int]] = {
        "lfo": 1,
        "filter": 2,
        "amp": 4,
        "arp": 8,
        "select": 0x20,
    }

    def act(self, action: str) -> None:
        """One scripted input: +lfo, -lfo, enc+16, kb=8000, key=3, cutoff=2000, shot."""
        name = action.lstrip("+-")
        down = action.startswith("+")
        if action == "shot":
            self.shots.append((self.ms, self.oled.render()))
        elif action.startswith("enc"):
            self.enc = (self.enc + int(action[3:])) & 0xFFFFFFFF
        elif action.startswith("kb="):
            self.kb = int(action[3:], 16)
            self.pots[15] = 52 if self.kb else 4095
        elif action.startswith(
            "key="
        ):  # touch keys by number, 0 = lowest; "key=" lets go
            self.kb = sum(0x8000 >> int(k) for k in action[4:].split("+") if k)
            self.pots[15] = 52 if self.kb else 4095
        elif action.startswith("cutoff="):
            self.pots[3] = int(action[7:])
        elif action.startswith("resonance="):
            self.pots[4] = int(action[10:])
        elif name in self.BUTTONS:
            self.keys = (
                self.keys | self.BUTTONS[name]
                if down
                else self.keys & ~self.BUTTONS[name]
            )
        elif name == "osc":
            self.idr[GPIOB] = (
                self.idr[GPIOB] & ~0x10 if down else self.idr[GPIOB] | 0x10
            )
        elif name == "settings":
            self.idr[GPIOA] = (
                self.idr[GPIOA] & ~0x400 if down else self.idr[GPIOA] | 0x400
            )
        else:
            sys.exit(f"error: --script: unknown action '{action}'")
        self.adc_refresh()

    def run(self, ms: int) -> None:
        pc = self.entry
        try:
            while self.ms < ms and not self.reset:
                while self.events and self.events[0][0] <= self.ms:
                    self.act(self.events.pop(0)[1])
                before = self.ms
                self.uc.emu_start(pc | 1, 0xFFFFFFFE, count=200_000)
                pc = self.uc.reg_read(UC_ARM_REG_PC)
                if self.ms == before:
                    self.ms += (
                        2  # no delay polled: 200k instructions is about 2 ms at 170 MHz
                    )
                # one audio block per 2 ms of simulated time
                while not self.reset and self.audio_ms < self.ms and self.audio_irq():
                    pass
        except UcError as e:
            self.log.append(
                f"{self.ms:5d} ms  CPU FAULT at pc={self.uc.reg_read(UC_ARM_REG_PC):#010x}: {e}"
            )


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("image", help="raw (unencrypted) application image")
    p.add_argument("--ms", type=int, default=400, help="simulated milliseconds to run")
    p.add_argument("--osc", action="store_true", help="hold the OSC button")
    p.add_argument("--settings", action="store_true", help="hold the SETTINGS button")
    p.add_argument(
        "--key", action="store_true", help="touch a keyboard key (lowest unless --kb)"
    )
    p.add_argument(
        "--kb",
        type=lambda s: int(s, 16),
        help="keyboard bits in hex, 8000 = lowest key",
    )
    p.add_argument("--wav", help="write the DAC output to this WAV file")
    p.add_argument(
        "--script",
        default="",
        help="timed inputs, 'ms action' separated by ';'. Actions: +lfo -lfo (press, release; "
        "also filter amp arp select osc settings), tap=lfo (press for 80 ms), enc+16 enc-8 "
        "(turn SELECT; 8 counts is one step in the examples), key=3 or key=0+4 (touch keys, "
        "0 = lowest; 'key=' lets go), cutoff=2000 resonance=0 (knobs), shot (print the display)",
    )
    p.add_argument(
        "--cutoff", type=int, help="CUTOFF knob reading, 0..4095 (default 4095)"
    )
    p.add_argument(
        "--resonance", type=int, help="RESONANCE knob reading, 0..4095 (default 0)"
    )
    a = p.parse_args()
    with open(a.image, "rb") as f:
        image = f.read()
    if not 8 <= len(image) <= 0x18000:
        sys.exit(
            f"error: {a.image} is {len(image)} bytes; an application image is 8..98304"
        )
    sp, pc = struct.unpack_from("<2I", image)
    if not (
        0x20000000 <= sp <= 0x20008000 and FLASH <= pc < FLASH + 0x18000 and pc & 1
    ):
        sys.exit(
            f"error: {a.image} does not start with a vector table (SP {sp:#x}, reset {pc:#x}).\n"
            "       The simulator takes the plain image (build/<app>/<app>.bin), not the\n"
            "       encrypted .cry.bin."
        )
    b = Board(image, a.osc, a.settings)
    for item in filter(None, (x.strip() for x in a.script.split(";"))):
        try:
            when, action = item.split()
            at = int(when)
        except ValueError:
            sys.exit(f"error: --script: '{item}' is not 'ms action'")
        if action.startswith("tap="):
            b.events += [(at, "+" + action[4:]), (at + 80, "-" + action[4:])]
        else:
            b.events.append((at, action))
    b.events.sort(key=lambda e: e[0])
    if a.cutoff is not None:
        b.pots[3] = max(0, min(4095, a.cutoff))
    if a.resonance is not None:
        b.pots[4] = max(0, min(4095, a.resonance))
    if a.key or a.kb:
        b.pots[15] = 52
        b.kb = a.kb or 0x8000
    b.run(a.ms)
    print("\n".join(b.log))
    o = b.oled
    print(
        f"ran {b.ms} ms; OLED: {len(o.cmds)} command bytes, {o.data_bytes} data bytes, display {'ON' if o.on else 'off'}"
    )
    print(f"first commands: {' '.join(f'{c:02X}' for c in o.cmds[:28])}")
    if b.dac:
        lo, hi = min(b.dac), max(b.dac)
        tail = b.dac[len(b.dac) // 2 :]
        ups = sum(1 for x, y in itertools.pairwise(tail) if x < 0x8000 <= y)
        print(
            f"DAC: {len(b.dac)} samples, range {lo:#06x}..{hi:#06x}, about {ups * 32000 / max(len(tail), 1):.0f} Hz; filter PWM {b.pwm}"
        )
        # Thumb-2 instructions average about 3 bytes; float code runs near 1.5 cycles each.
        cycles = b.irq_worst / 3 * 1.5
        print(
            f"audio interrupt: worst block about {cycles / 1000:.0f}k cycles of 340k available ({cycles / 3400:.0f} %, rough estimate)"
        )
        if a.wav:
            import wave

            with wave.open(a.wav, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(32000)
                w.writeframes(b"".join(struct.pack("<h", s - 0x8000) for s in b.dac))
            print("wrote", a.wav)
    for when, picture in [*b.shots, (b.ms, o.render())]:
        print(f"+{f' {when} ms ':-^128}+")
        for line in picture.splitlines() or [""]:
            print("|" + line.ljust(128) + "|")
    print("+" + "-" * 128 + "+")
    sys.exit(0)


if __name__ == "__main__":
    cli.run(main)
