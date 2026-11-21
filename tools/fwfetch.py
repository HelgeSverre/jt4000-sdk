"""Download the factory JT-4000 Micro firmware from Music Tribe's update server.

The firmware is Behringer's, so this repository does not carry it. This tool fetches it
the way SynthTribe does: an anonymous token from the cloud API, then the file list.

The token handshake uses a secret that ships inside SynthTribe (proof = SHA-256 of
secret + proofSalt; the answer is SHA-256 of secret + salt). It is included below, so
SynthTribe does not have to be installed.

    uv run tools/fwfetch.py              # latest version -> firmware/
    uv run tools/fwfetch.py --all        # every released version
    uv run tools/fwfetch.py --list
    uv run tools/fwfetch.py --all --decrypt   # also write firmware/decrypted/ (for Ghidra)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

API = "https://cloud.midasconsoles.com/api/v2"
CLIENT = "63dbaa87-15b1-4681-a47f-34ea5bfd10df"  # SynthTribe's client id
MODEL = "behringer/jt-4000-micro/jt-4000-micro"
ROOT = Path(__file__).resolve().parent.parent
# The 512-byte handshake secret, as stored (in hex) in the SynthTribe 2.9.0 executable.
# Set JT_CLOUD_SECRET to override it if a later SynthTribe changes it.
SECRET = (
    "7c2dcdf34027c6858d21b9c1e6d9e907e0bd9502bfa603abcc39290ee57056d5bb857887861cad5c"
    "34ddc51e3d446c01ec24775b9685c66dde507b300133303ecdca3c5b3d9b5d39208d5c30d8e29173"
    "134b05d858f15819f0ee5e9205bb47d523c9f6ea4bda0bf2cdb3470c7cd8b9451121c4b3744bc2a6"
    "2f56808b34fb2131042438e9ced8bd809706e0eab846a6930da31ce92ad6dbcc82d342ccf2f0d4a3"
    "cf8171a93d4919ac729f13484c63fe8f6d4da3dc15aefe1a0bb11dc149704bcbb9e6625d831267ce"
    "3f80cb3266567fc5b1b386757e08bd8b5200ed35c4ea15fd85c8e481b33421184f3e9a94a014c41d"
    "a295a32229239dd34def5aa8b53fddbf3da755a9dc19240a1b398b30f02cf3233fc484da53e6ea6d"
    "1c49abe1aa1d5d049903491ad73d5843f1f3f0110c3d4df38f70459846332166f57664cc2c45c098"
    "465223eb381c78565eb3b696c1a919850fff5d424115458b30033fe90525f28ef7891b4c43339d7b"
    "ca5ddeaddc03c51b4d877b469dd9d4d1b697eb823e2e8bc22134803bd13e664282d67f4849e97a1b"
    "6ebd409a43d1dcc67b0cb4a0cf6a576601289dc7686004b5085d957a022bd014526e90c8af2b67d0"
    "ec64e9d24e17296c8105c34253777aa5535563a824f10bf21d1dbf0a3de20c601c20d0636747b694"
    "e0ead0b9e8bd95bb58104b4b3db7d7a8405b30da959608d60e42d1d29c680ff6"
)


def secret() -> bytes:
    return bytes.fromhex(os.environ.get("JT_CLOUD_SECRET", SECRET))


def call(path: str, body: dict | None = None, token: str | None = None):
    headers = {"User-Agent": "Mozilla/5.0", "Accept": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(API + path, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        sys.exit(f"{path}: HTTP {e.code} {e.read()[:200].decode(errors='replace')}")


def login(key: bytes) -> str:
    ch = call("/auth/tokens", {"client": CLIENT})
    c = ch["client"]
    if hashlib.sha256(key + bytes.fromhex(c["proofSalt"])).hexdigest() != c["proof"]:
        sys.exit(
            "the server's proof does not match: wrong secret, or the handshake changed"
        )
    answer = hashlib.sha256(key + bytes.fromhex(c["salt"])).hexdigest()
    r = call(ch["responsePath"], {"clientResponse": answer})
    if not r.get("success"):
        sys.exit(f"login refused: {r}")
    return r["token"]


def download(rec: dict, out: Path) -> Path:
    dst = out / rec["filename"]
    if dst.is_file() and hashlib.md5(dst.read_bytes()).hexdigest() == rec["checksum"]:
        print(f"{rec['version']}: {dst} already there")
        return dst
    # The link is pre-signed and short-lived; it takes no extra headers.
    with urllib.request.urlopen(rec["s3Url"], timeout=60) as r:
        blob = r.read()
    if hashlib.md5(blob).hexdigest() != rec["checksum"] or len(blob) != rec["bytes"]:
        sys.exit(
            f"{rec['version']}: downloaded file does not match the server's checksum"
        )
    out.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(blob)
    print(f"{rec['version']}: wrote {dst} ({len(blob)} bytes, md5 ok)")
    return dst


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument(
        "--all", action="store_true", help="every released version, not only the latest"
    )
    p.add_argument(
        "--list", action="store_true", help="show what the server has and stop"
    )
    p.add_argument("--decrypt", action="store_true", help="also write decrypted copies")
    p.add_argument("--out", type=Path, default=ROOT / "firmware")
    a = p.parse_args()

    token = login(secret())
    recs = call(f"/firmware/{MODEL}", token=token)
    if a.list:
        for r in recs:
            print(
                f"{r['version']}  {r['createdAt'][:10]}  {r['filename']}  md5 {r['checksum']}"
            )
        return
    for rec in recs if a.all else recs[:1]:
        path = download(rec, a.out)
        if a.decrypt:
            from fwcrypt import cbc_decrypt
            from fwpack import KEY

            plain = (
                a.out / "decrypted" / f"JT-4000_V{rec['version'].replace('.', '')}.bin"
            )
            plain.parent.mkdir(parents=True, exist_ok=True)
            plain.write_bytes(cbc_decrypt(KEY, path.read_bytes()))
            print(f"{rec['version']}: wrote {plain}")


if __name__ == "__main__":
    main()
