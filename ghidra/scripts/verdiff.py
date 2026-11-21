"""Compare two exported decompilations function by function, ignoring address shifts.

uv run python ghidra/scripts/verdiff.py 114 115 [--show N]
"""

import difflib
import hashlib
import re
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "out"
NAMES = Path(__file__).resolve().parent.parent


def load(v):
    names = set()
    nf = NAMES / f"names-v{v}.tsv"
    if nf.exists():
        names = {
            l.split("\t")[1]
            for l in nf.read_text().splitlines()
            if l and not l.startswith("#")
        }
    text = (OUT / f"v{v}" / "decomp.c").read_text()
    funcs = []
    for m in re.finditer(
        r"// ===== (\S+) @ ([0-9a-f]+) =====\n(.*?)(?=\n// ===== |\Z)", text, re.DOTALL
    ):
        body = re.sub(r"/\*.*?\*/", "", m.group(3), flags=re.DOTALL)
        body = re.sub(r"\n\s*\n", "\n", body)
        norm = re.sub(
            r"\b(FUN|DAT|LAB|PTR|UNK|_DAT|switchD|caseD|code_r0x)_[0-9a-f]+\b",
            "SYM",
            body,
        )
        norm = re.sub(r"\b0x(0800|0801|2000)[0-9a-f]{4}\b", "ADDR", norm)
        for n in names:
            norm = re.sub(rf"\b{re.escape(n)}\b", "SYM", norm)
        norm = re.sub(
            r"\b[a-z]+Var\d+\b|\blocal_\w+\b|\b\w*Stack_\w+\b|\bextraout_\w+\b",
            "V",
            norm,
        )
        funcs.append(
            (m.group(1), m.group(2), norm, hashlib.md5(norm.encode()).hexdigest())
        )
    return funcs


def main():
    a, b = sys.argv[1], sys.argv[2]
    show = int(sys.argv[sys.argv.index("--show") + 1]) if "--show" in sys.argv else 0
    fa, fb = load(a), load(b)
    sm = difflib.SequenceMatcher(
        None, [f[3] for f in fa], [f[3] for f in fb], autojunk=False
    )
    same = sum(j2 - j1 for op, _, _, j1, j2 in sm.get_opcodes() if op == "equal")
    print(
        f"v{a}: {len(fa)} functions, v{b}: {len(fb)} functions, identical after normalising: {same}"
    )
    for op, i1, i2, j1, j2 in sm.get_opcodes():
        if op == "equal":
            continue
        old = [f"{f[0]}@{f[1]}({len(f[2])})" for f in fa[i1:i2]]
        new = [f"{f[0]}@{f[1]}({len(f[2])})" for f in fb[j1:j2]]
        print(
            f"{op:8s} v{a}: {' '.join(old) or '-'}\n         v{b}: {' '.join(new) or '-'}"
        )
        if show and i2 - i1 == j2 - j1:
            for x, y in zip(fa[i1:i2], fb[j1:j2]):
                d = [
                    l
                    for l in difflib.unified_diff(
                        x[2].splitlines(), y[2].splitlines(), lineterm="", n=1
                    )
                    if l[:1] in "+-" and l[:3] not in ("+++", "---")
                ]
                for l in d[:show]:
                    print("            " + l[:150])
                if len(d) > show:
                    print(f"            ... {len(d) - show} more changed lines")


main()
