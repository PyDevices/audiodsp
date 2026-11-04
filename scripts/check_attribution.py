#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Fail if a ported source has lost its upstream copyright notice.

Most C/H files under src/ are ports of CircuitPython (and through it,
MicroPython) sources. MIT requires the original copyright notice to travel
with the code, and upstream SPDX-headers its whole tree and lints for it.

Forty-five ported files had silently dropped that notice before 2026-09-02,
leaving a bare PyDevices copyright over code written by a dozen named
people. Nothing caught it, because the only signal is a prose sentence in
the header. This turns that sentence into the check: a file that says it
was ported must also say whose work it was.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# The claim we hold files to: any header that says it came from upstream.
# "micropython-vst3" is our own repo, not upstream - excluded deliberately.
PORTED = re.compile(
    r"ported\s+(verbatim\s+)?from\s+(circuitpython|micropython(?!-vst3))", re.I
)


def leading_comment(text: str) -> str:
    lines = text.splitlines()
    out = []
    for line in lines:
        if line.startswith("//") or not line.strip():
            out.append(line.lstrip("/ ").rstrip())
        else:
            break
    return " ".join(out)


def main() -> int:
    listed = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "src/*.c", "src/*.h"],
        capture_output=True, text=True, check=True,
    ).stdout.split()
    offenders = []
    for rel in listed:
        text = (ROOT / rel).read_text(encoding="utf-8", errors="replace")
        if not PORTED.search(leading_comment(text)):
            continue
        if "SPDX-FileCopyrightText" not in text:
            offenders.append(rel)
    if offenders:
        print("These files say they were ported from upstream but carry no")
        print("SPDX-FileCopyrightText line. Copy the upstream file's copyright")
        print("lines into the header, above SPDX-License-Identifier, and add")
        print("PyDevices' own. See NOTICE.\n")
        for rel in offenders:
            print(f"  {rel}")
        return 1
    print(f"attribution ok: every ported file under src/ names its authors "
          f"({len(listed)} files checked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
