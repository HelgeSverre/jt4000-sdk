"""Talk to the Behringer JT-4000 Micro over MIDI SysEx.

Framing (from SynthTribe 2.9.0, General_SysExParser::packMessage):

    F0 00 20 32 <m0> <m1> <m2> [<devid> if protocol 3] <payload...> F7

JT-4000 uses protocol 2 with model bytes 00 01 38 (Devices/JT-4000 MICRO/config.json),
so no device-id byte is sent. The first payload byte is the command; a reply's command
is usually request + 1.

Usage:
    uv run tools/sysex.py ports
    uv run tools/sysex.py version
    uv run tools/sysex.py send 08          # raw payload, header and F7 added
    uv run tools/sysex.py listen
"""

from __future__ import annotations

import argparse
import sys
import time

import cli
import mido

MANUFACTURER = (0x00, 0x20, 0x32)  # Behringer / Music Tribe
MODEL = (0x00, 0x01, 0x38)  # JT-4000 MICRO
PORT_HINTS = ("JT-4000", "JT4000", "SATURN")

# Commands seen in SynthTribe's General_ProtocolZS dispatch table.
CMD_GET_FW_VERSION = 0x08
REPLY_NAMES = {
    0x05: "model code",
    0x07: "model name",
    0x09: "firmware version",
    0x0D: "CV value",
    0x5A: "old update",
    0x76: "entire parameter",
    0x78: "sequencer parameter",
    0x7E: "poly chain",
}

HEADER = (*MANUFACTURER, *MODEL)  # mido strips F0/F7


def hexs(data) -> str:
    return " ".join(f"{b:02X}" for b in data)


def find_port(names: list[str], wanted: str | None) -> str:
    if wanted:
        for n in names:
            if wanted.lower() in n.lower():
                return n
        sys.exit(
            f"error: no MIDI port matching {wanted!r}. Available: {names or 'none'}"
        )
    for n in names:
        if any(h.lower() in n.lower() for h in PORT_HINTS):
            return n
    sys.exit(
        f"error: JT-4000 not found among the MIDI ports: {names or 'none'}\n"
        "       Connect it and switch it on (not in update mode), or name a port with --port."
    )


def open_ports(port: str | None):
    out_name = find_port(mido.get_output_names(), port)
    in_name = find_port(mido.get_input_names(), port)
    return mido.open_output(out_name), mido.open_input(in_name), out_name


def build(payload: list[int]) -> mido.Message:
    if any(b > 0x7F for b in payload):
        sys.exit("error: SysEx payload bytes must be 00-7F")
    return mido.Message("sysex", data=[*HEADER, *payload])


def recv_reply(inp, timeout: float, verbose: bool = True) -> list[int] | None:
    """Return the payload (after header) of the first SysEx from the device."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for msg in inp.iter_pending():
            if msg.type != "sysex":
                continue
            data = list(msg.data)
            if verbose:
                print(f"<- F0 {hexs(data)} F7")
            if tuple(data[: len(HEADER)]) == HEADER:
                return data[len(HEADER) :]
        time.sleep(0.005)
    return None


def transact(port: str | None, payload: list[int], timeout: float) -> list[int] | None:
    out, inp, name = open_ports(port)
    with out, inp:
        msg = build(payload)
        print(f"port: {name}")
        print(f"-> F0 {hexs(msg.data)} F7")
        out.send(msg)
        return recv_reply(inp, timeout)


def cmd_ports(_args) -> None:
    print("inputs: ", *mido.get_input_names(), sep="\n  ")
    print("outputs:", *mido.get_output_names(), sep="\n  ")


def cmd_version(args) -> None:
    reply = transact(args.port, [CMD_GET_FW_VERSION], args.timeout)
    if reply is None:
        sys.exit(
            f"error: no reply within {args.timeout} s (is this the stock firmware?)"
        )
    # Device replies 09 <xx> <major> <minor> <patch>. SynthTribe's handler gets
    # [xx, major, minor, patch] after the command byte and reads indices 1..3.
    # xx was 00 on a JT-4000 running 1.1.5 (likely device id / status).
    if len(reply) == 5 and reply[0] == CMD_GET_FW_VERSION + 1:
        print(f"firmware {reply[2]}.{reply[3]}.{reply[4]} (byte1={reply[1]:02X})")
    else:
        print(f"unexpected reply payload: {hexs(reply)}")


def cmd_send(args) -> None:
    try:
        payload = [int(b, 16) for b in " ".join(args.bytes).replace(",", " ").split()]
    except ValueError as e:
        sys.exit(f"error: the payload must be hex bytes such as '08' or '01 7F' ({e})")
    if not payload:
        sys.exit("error: empty payload")
    reply = transact(args.port, payload, args.timeout)
    if reply is None:
        print("no reply")
    else:
        name = REPLY_NAMES.get(reply[0], "?") if reply else "empty"
        print(f"reply cmd {reply[0]:02X} ({name}): {hexs(reply[1:])}")


# General_GeneralController::requestReceive(int), driven by the Receive buttons in
# qml/general/SynthToolControlDumpReceive.qml (order: Active Preset, All Presets,
# Active Pattern, All Patterns).
DUMP_REQUESTS = {
    "preset": [0x00, 0x00],
    "presets": [0x00, 0x20],
    "pattern": [0x01, 0x00],
    "patterns": [0x01, 0x10],
}


def collect(inp, first_timeout: float, idle: float) -> list[list[int]]:
    """Collect SysEx messages until `idle` seconds pass without a new one."""
    msgs: list[list[int]] = []
    deadline = time.monotonic() + first_timeout
    while time.monotonic() < deadline:
        for msg in inp.iter_pending():
            if msg.type == "sysex":
                msgs.append(list(msg.data))
                deadline = time.monotonic() + idle
        time.sleep(0.002)
    return msgs


def cmd_dump(args) -> None:
    out, inp, name = open_ports(args.port)
    with out, inp:
        msg = build(DUMP_REQUESTS[args.what])
        print(f"port: {name}")
        print(f"-> F0 {hexs(msg.data)} F7")
        out.send(msg)
        msgs = collect(inp, args.timeout, args.idle)
    print(f"received {len(msgs)} SysEx message(s), {sum(map(len, msgs))} bytes")
    for i, data in enumerate(msgs):
        body = hexs(data)
        print(
            f"[{i}] len={len(data) + 2}: F0 {body[:120]}{' ...' if len(body) > 120 else ''}"
        )
    if args.out:
        with open(args.out, "wb") as f:
            for data in msgs:
                f.write(bytes([0xF0, *data, 0xF7]))
        print(f"wrote {args.out}")


def checksum(data: list[int] | bytes) -> int:
    """Dump checksum: (sum + cs) & 0x7F == 0 over the preset bytes."""
    return (-sum(data)) & 0x7F


def cmd_upload(args) -> None:
    """Send a .syx file to the device unchanged, like SynthTribe's Send buttons.

    General_GeneralController::onLoadDumpReceive checks F0 00 20 32 <model> ... F7
    and the command byte (15-17 single preset, 10-12 all presets), then sends the
    raw bytes in 255-byte chunks. The device must be in SYSEX RECEIVE mode.
    """
    with open(args.file, "rb") as f:
        raw = f.read()
    if raw[:1] != b"\xf0" or raw[-1:] != b"\xf7" or tuple(raw[1:7]) != HEADER:
        sys.exit(f"error: {args.file}: not a JT-4000 SysEx file")
    name = find_port(mido.get_output_names(), args.port)
    with mido.open_output(name) as out:
        for msg in mido.parse_all(raw):
            if msg.type != "sysex" or len(msg.data) < 7:
                sys.exit(
                    f"error: {args.file}: contains something other than JT-4000 SysEx"
                )
            print(f"-> {msg.type} {len(msg.bin())} bytes, cmd {msg.data[6]:02X}")
            out.send(msg)
            time.sleep(0.05)


def cmd_listen(args) -> None:
    name = find_port(mido.get_input_names(), args.port)
    print(f"listening on {name} (Ctrl+C to stop)")
    with mido.open_input(name) as inp:
        try:
            for msg in inp:
                if msg.type == "sysex":
                    print(f"<- F0 {hexs(msg.data)} F7")
                elif args.all:
                    print(f"<- {msg}")
        except KeyboardInterrupt:
            pass


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port", help="substring of the MIDI port name")
    p.add_argument(
        "--timeout", type=float, default=1.0, help="reply timeout in seconds"
    )
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ports", help="list MIDI ports").set_defaults(fn=cmd_ports)
    sub.add_parser("version", help="query firmware version").set_defaults(
        fn=cmd_version
    )
    s = sub.add_parser("send", help="send a raw payload (hex), header and F7 added")
    s.add_argument("bytes", nargs="+")
    s.set_defaults(fn=cmd_send)
    d = sub.add_parser("dump", help="request a preset/pattern dump")
    d.add_argument("what", choices=DUMP_REQUESTS)
    d.add_argument(
        "--idle", type=float, default=0.5, help="stop after this much silence"
    )
    d.add_argument("-o", "--out", help="write raw .syx file")
    d.set_defaults(fn=cmd_dump)
    u = sub.add_parser("upload", help="send a .syx dump file to the device")
    u.add_argument("file")
    u.set_defaults(fn=cmd_upload)
    li = sub.add_parser("listen", help="print incoming SysEx")
    li.add_argument("--all", action="store_true", help="also print non-SysEx messages")
    li.set_defaults(fn=cmd_listen)
    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    cli.run(main)
