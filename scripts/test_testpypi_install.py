#!/usr/bin/env python3
"""Smoke a fresh pydevices-audiodsp install from TestPyPI.

    scripts/test_testpypi_install.py [VERSION] [--skip-render]

`docs/building-wheels.md` names this as a post-release step to run on fresh
Linux and Windows hosts. Two things used to stop it being one (audiodsp#72):
the version was frozen into the source as `0.0.1`, so it smoked a 2026-08-25
build whichever release it was run for, and its embedded snippet used
`array(...)` with no import, so it raised `NameError` before it could assert
anything -- after two lines of real work had already printed, which reads
like a packaging failure rather than a typo in the checker.

VERSION now defaults to this checkout's `VERSION` file, so the script follows
the release it is run beside. Pass one explicitly to smoke any other.

Two environments are built, each from scratch, because they answer different
questions: the bare install is what someone who wants the audio nodes gets,
and the `[render]` install is the only check anywhere that the extra's name is
still spelled the way `pyproject.toml` declares it (audiodsp#35). `--skip-render`
drops the second if numpy's download is not welcome.

`--extra-index-url` is not optional: TestPyPI alone cannot resolve numpy, and
a run without it fails in the dependency resolver with nothing to do with
this package (audiodsp#30).
"""

import subprocess
import sys
import tempfile
import venv
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

INDEX = ("--index-url", "https://test.pypi.org/simple/",
         "--extra-index-url", "https://pypi.org/simple/")

#: The bare install's smoke: a synth renders a full block, and a two-sample
#: `RawSample` reports GET_BUFFER_DONE rather than more-data.
BARE = """
from array import array
import audiocore, synthio
s = synthio.Synthesizer(sample_rate=8000)
s.press(69)
r, b = audiocore.get_buffer(s)
assert r == 1 and b.format == 'B' and len(b) == 512, (r, b.format, len(b))
assert audiocore.get_buffer(audiocore.RawSample(array('h', [1, 2])))[0] == 0
print('bare install OK:', len(b), 'bytes of synth output')
"""

#: The `[render]` install's smoke. `audiorender` is numpy-throughout, so
#: importing it is what proves the extra actually pulled numpy in.
RENDER = """
import numpy
import audiorender
from audiorender import events, tempo, wav          # noqa: F401
print('[render] install OK: numpy', numpy.__version__)
"""


def smoke(label, requirement, code):
    """Build a throwaway venv, install `requirement` into it, run `code`."""
    with tempfile.TemporaryDirectory(prefix="audiodsp-testpypi-") as directory:
        root = Path(directory)
        venv.EnvBuilder(with_pip=True).create(root)
        python = root / ("Scripts/python.exe" if sys.platform == "win32"
                         else "bin/python")
        print("== %s: installing %s" % (label, requirement))
        # Run from the throwaway directory, never the caller's: inside a
        # source checkout, pip reads the tree as an already-installed
        # distribution and the smoke import resolves to the unbuilt sources.
        subprocess.run([str(python), "-m", "pip", "install", *INDEX,
                        requirement], check=True, cwd=root)
        subprocess.run([str(python), "-c", code], check=True, cwd=root)


def main(argv):
    arguments = [a for a in argv if not a.startswith("--")]
    unknown = [a for a in argv if a.startswith("--") and a != "--skip-render"]
    if unknown or len(arguments) > 1:
        raise SystemExit(__doc__.strip().splitlines()[2].strip())

    version = arguments[0] if arguments else \
        (ROOT / "VERSION").read_text().strip()
    print("smoking pydevices-audiodsp %s from TestPyPI" % version)

    smoke("bare", "pydevices-audiodsp==%s" % version, BARE)
    if "--skip-render" in argv:
        print("== [render]: skipped")
    else:
        smoke("[render]", "pydevices-audiodsp[render]==%s" % version, RENDER)
    print("all TestPyPI install checks passed for %s" % version)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
