#!/usr/bin/env python3
"""Fail if this repo's C has learned about a platform.

audioif is the DSP half of the audio stack. The same sources build as a
MicroPython usermod, as a CPython extension and inside CircuitPython, on
ESP32-P4, ESP32-S3, unix, Windows and WebAssembly -- and the way that stays
true is that nothing in here knows what a thread, a mutex, a clock or an I2S
channel is. Those arrive through ``src/shared/audioif_port.h`` from a driver
that lives in another repo.

That is a rule you cannot see: a single ``#include <pthread.h>`` compiles
happily on the box you are working on and only bites the next port. So it is
checked.

Run it::

    python3 tools/check_portable.py            # the whole repo
    python3 tools/check_portable.py src/shared # or a subtree

Exit status is 0 when clean, 1 when something platform-shaped is in the C.
Comments and string literals are stripped first, so a file may explain the
rule without breaking it.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Where our C lives. Everything under these, .c and .h.
ROOTS = ("src",)

# Third-party code we carry but did not write. Nothing qualifies today --
# ulab, the mp3 decoder and CircuitPython itself are sibling checkouts, not
# copies in here -- so this is a list with a name rather than a list with
# entries, and an entry added later has to earn its line.
VENDORED: tuple[str, ...] = ()

# The headers. `esp_` and `driver/` catch the whole IDF surface; `sdkconfig`
# catches the build's own config, which is the other way an IDF fact gets in.
BANNED_INCLUDES = (
    r"freertos/",
    r"esp_[A-Za-z0-9_]*\.h",
    r"driver/",
    r"sdkconfig",
    r"pthread\.h",
    r"windows\.h",
    r"process\.h",
    r"unistd\.h",
    r"sys/time\.h",
    r"sched\.h",
)

INCLUDE_RE = re.compile(
    r"^\s*#\s*include\s*[<\"](?P<path>[^>\"]+)[>\"]", re.MULTILINE)

# The calls. An include is the usual way in, but not the only one: a
# declaration copied by hand, or a symbol picked up from a header something
# else dragged in, gets here without one.
BANNED_CALLS = (
    r"clock_gettime",
    r"nanosleep",
    r"\busleep\b",
    r"\bsched_yield\b",
    r"\bpthread_[a-z_]+",
    r"\bxTask[A-Za-z]+",
    r"\bvTask[A-Za-z]+",
    r"\bxSemaphore[A-Za-z]+",
    r"\bportYIELD\b",
    r"\bportTICK[A-Z_]*",
    r"\besp_timer_get_time\b",
    r"\besp_rom_delay_us\b",
    r"\bi2s_channel_[a-z_]+",
    r"\bCreateThread\b",
    r"\b_beginthreadex\b",
    r"\bQueryPerformance[A-Za-z]+",
    r"\bGetCurrentThreadId\b",
    r"\bInitializeCriticalSection\b",
    r"\bEnterCriticalSection\b",
    r"\bSwitchToThread\b",
    r"\bInterlocked[A-Za-z]+",
    r"\bIRAM_ATTR\b",
    r"\bMALLOC_CAP_[A-Z]+",
)

CALL_RE = re.compile("|".join(BANNED_CALLS))


def strip_noise(text: str) -> str:
    """Blank out comments and string literals, keeping line numbers intact.

    A file is allowed to say *why* it must not call clock_gettime.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif ch == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif ch in "\"'":
            quote = ch
            j = i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def show(path: Path) -> str:
    """The path as a reader would name it, even when it is not in this tree.

    `check_portable.py /somewhere/else/src` is how a past revision gets
    checked -- `git archive main src | tar -x` somewhere and point this at it
    -- so the path may be anywhere.
    """
    try:
        return path.relative_to(REPO).as_posix()
    except ValueError:
        return path.as_posix()


def is_vendored(path: Path) -> bool:
    rel = show(path)
    return any(rel.startswith(v) for v in VENDORED)


def sources(targets: list[str]) -> list[Path]:
    roots = [Path(t) if Path(t).is_absolute() else REPO / t for t in targets]
    if not roots:
        roots = [REPO / r for r in ROOTS]
    found: list[Path] = []
    for root in roots:
        if root.is_file():
            found.append(root)
            continue
        for suffix in ("*.c", "*.h"):
            found.extend(sorted(root.rglob(suffix)))
    return [p for p in found if not is_vendored(p)]


def check(path: Path) -> list[tuple[int, str]]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    text = strip_noise(raw)
    hits: list[tuple[int, str]] = []
    for match in INCLUDE_RE.finditer(text):
        header = match.group("path")
        for pattern in BANNED_INCLUDES:
            if re.search(pattern, header):
                line = text.count("\n", 0, match.start()) + 1
                hits.append((line, f"#include <{header}>"))
                break
    for match in CALL_RE.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        hits.append((line, match.group(0)))
    return sorted(set(hits))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*",
        help="files or directories to check (default: src/)")
    parser.add_argument("-q", "--quiet", action="store_true",
        help="say nothing when clean")
    args = parser.parse_args()

    files = sources(args.paths)
    problems = 0
    for path in files:
        for line, what in check(path):
            print(f"{show(path)}:{line}: platform code in portable C: {what}")
            problems += 1

    if problems:
        print()
        print(f"{problems} platform reference(s) in {len(files)} files.")
        print("The thread, the mutex, the clock, the pacing and the sink come")
        print("from shared/audioif_port.h. Put it in the driver instead.")
        return 1
    if not args.quiet:
        print(f"{len(files)} files, no platform code. "
              f"audioif still builds everywhere.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
