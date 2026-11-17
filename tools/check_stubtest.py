#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Check each module's type stub against the CPython module that is installed.

mypy proves a stub is well formed. mypy's stubtest goes further and imports the
module, then compares what the stub says with what is really there: a parameter
the module takes by keyword that the stub makes positional, a method the stub
lacks, a default that differs. The stubs describe the module built into
MicroPython and CircuitPython, so wherever the CPython twin differs on purpose
or by accident, ``tools/stubs/stubtest_allowlist.txt`` lists that one name and
says why. Anything stubtest finds that the list doesn't name fails the run, and
so does a list entry that no longer matches anything.

Run it from a checkout, with the wheel (or an editable install) in the
interpreter you use::

    python3 tools/check_stubtest.py                # every module
    python3 tools/check_stubtest.py audiofilters   # one module

The stubs it checks are the ones in ``src/``, not the copies a wheel carries, so
an edit shows at once. Exit status is stubtest's: 0 when clean.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ALLOWLIST = ROOT / "tools" / "stubs" / "stubtest_allowlist.txt"


def stub_files() -> dict[str, Path]:
    """Map each module name to the stub file that describes it."""
    return {path.stem: path for path in sorted((ROOT / "src").glob("*/*.pyi"))}


def main(argv: list[str]) -> int:
    stubs = stub_files()
    modules = argv or sorted(stubs)
    unknown = [name for name in modules if name not in stubs]
    if unknown:
        print("no stub for: " + ", ".join(unknown), file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory() as tmp:
        # mypy reads a stub folder named <module> from MYPYPATH. The wheel's
        # <module>-stubs folders are only found when they are installed, and
        # an installed one would hide an edit made here.
        search = Path(tmp) / "stubs"
        work = Path(tmp) / "work"
        work.mkdir()
        for name in stubs:
            folder = search / name
            folder.mkdir(parents=True)
            (folder / "__init__.pyi").write_bytes(stubs[name].read_bytes())
        env = dict(os.environ, MYPYPATH=str(search))
        command = [sys.executable, "-m", "mypy.stubtest"]
        if len(modules) == len(stubs):
            # Unused entries only mean something when every module is checked.
            command += ["--allowlist", str(ALLOWLIST)]
        else:
            command += ["--allowlist", str(ALLOWLIST), "--ignore-unused-allowlist"]
        # Run from an empty folder so nothing in this checkout shadows the
        # installed modules.
        return subprocess.call(command + modules, cwd=work, env=env)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
