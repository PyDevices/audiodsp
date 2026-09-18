#!/usr/bin/env python3
"""What mpvst's soundtrack pieces sound like, against a stored baseline.

    capture_render_reference.py --capture
    capture_render_reference.py --verify [--tolerance-db 1.0]

Renders every piece through mpvst's own offline renderer and records the
master WAV's hash, the analysis report, and the levels parsed out of it. The
stored fixture is the acceptance baseline for the cutover in which those
pieces stopped running private copies of the DSP and started importing
`audioinstruments` and `audioeffects`; a piece that moves after a change here
is this repository's DSP being heard in real music.

The renderer moved out from under this script and was repointed in
audioif#88. It used to be `micropython-vst3/tools/render_preview.py`. Three
things changed and none of them are in this repository: the checkout is
`mpvst` now (PyDevices/mpvst), the soundtrack is `examples/soundtrack/`, and
mpvst `ef0bed5` moved the renderer to `examples/soundtrack/composer/preview.py`
beside `harness.py`. It is invoked the way mpvst documents it
(`preview.py --piece NAME out.wav`) and prints the same report, because both
the old and the new renderer are thin shims over audioif's own `audiorender`
- so `normalize()` and the three line parsers below read it unchanged.

The interpreter that renders (`--python`; this one by default) needs numpy,
which the renderer imports, and `pydevices-audioif` installed - mpvst imports
audioif from wherever it is installed, never from a sibling path. It also
needs `audioinstruments` and `audioeffects`, which live in the audiocomponents
repository (https://github.com/PyDevices/audiocomponents). Either install them
into that interpreter or pass `--components-lib <audiocomponents checkout>/lib`.

`preview.py` loads an instrument the way the sidecar does - through the
bundle's `mpvst_instrument_adapter` - so it wants MPVST installed. Rather than
require a build, this script points `MPVST_BUNDLE` at mpvst's own `lib/`, which
is the directory the install is staged from and holds the adapters and nothing
else. That matters: a staged bundle also carries *copies* of the component
packages, and `harness.py` puts the bundle ahead of PYTHONPATH, so pointing at
a real install would silently render `--components-lib` inert and grade a
stale copy. Set `MPVST_BUNDLE` yourself to override.
"""

import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WORKSPACE = ROOT.parent
GOLDEN = HERE / "golden" / "vst3_render_reference.json"

DEFAULT_MPVST = WORKSPACE / "mpvst"

#: Where the pieces are, and what renders one, inside an mpvst checkout.
SOUNDTRACK = ("examples", "soundtrack")
PREVIEW = SOUNDTRACK + ("composer", "preview.py")

#: The two things in the report that are not reproducible: how long the render
#: took, and where this script happened to put the WAV. The elapsed figure may
#: be negative - the renderer subtracts two wall-clock readings, and under WSL
#: the clock resyncs backwards mid-render often enough to see it.
ELAPSED = re.compile(r"\((-?\d+\.\d+)s\)")
RENDER_SECONDS = re.compile(r"^(.*: [\d.]+ s song, )[\d.]+( s render.*)$",
                            re.MULTILINE)
WROTE = re.compile(r"^wrote .*$", re.MULTILINE)

# Read off the *normalized* report, so the elapsed time is already a placeholder.
TRACK_LINE = re.compile(
    r"^  (\S.*?)\s+raw_peak=([\d.]+) mixed_peak=([\d.]+) \(-\.-s\)",
    re.MULTILINE)
SECTION_LINE = re.compile(
    r"^  (\S.*?)\s+rms=\s*(-?[\d.]+) dBFS\s+hp150=\s*(-?[\d.]+) dBFS"
    r"\s+peak=\s*(-?[\d.]+) dBFS", re.MULTILINE)
MASTER_LINE = re.compile(r"^master peak [\d.]+ \((-?[\d.]+) dBFS\)",
                         re.MULTILINE)
SIMULTANEOUS = re.compile(r"^max simultaneous tracks: (\d+)", re.MULTILINE)


def pieces(mpvst):
    """Every piece in the soundtrack - a directory with a composition.py."""
    soundtrack = Path(mpvst).joinpath(*SOUNDTRACK)
    if not soundtrack.is_dir():
        raise SystemExit("no soundtrack at %s - pass --mpvst" % soundtrack)
    return sorted(entry.name for entry in soundtrack.iterdir()
                  if (entry / "composition.py").is_file())


def normalize(report):
    report = ELAPSED.sub("(-.-s)", report)
    report = RENDER_SECONDS.sub(r"\g<1>-.-\g<2>", report)
    return WROTE.sub("wrote <scratch>", report)


def levels(report):
    """The numbers worth comparing with a tolerance, by name."""
    found = {}
    for name, raw_peak, mixed_peak in TRACK_LINE.findall(report):
        found["track %s raw_peak" % name] = float(raw_peak)
        found["track %s mixed_peak" % name] = float(mixed_peak)
    for name, rms, hp150, peak in SECTION_LINE.findall(report):
        found["section %s rms" % name] = float(rms)
        found["section %s hp150" % name] = float(hp150)
        found["section %s peak" % name] = float(peak)
    master = MASTER_LINE.search(report)
    if master:
        found["master peak"] = float(master.group(1))
    return found


def adapter_bundle(mpvst, scratch):
    """A bundle layout holding mpvst's adapters and nothing else.

    `harness.py` wants `<bundle>/Contents/<arch>` and puts it at the front of
    sys.path. mpvst's `lib/` is what an install stages its adapters from, and
    unlike an installed bundle it carries no component packages - so this
    leaves `--components-lib` the only `audioinstruments` on the path.
    """
    contents = Path(scratch) / "MPVST.vst3" / "Contents"
    contents.mkdir(parents=True, exist_ok=True)
    os.symlink(str(Path(mpvst).resolve() / "lib"), str(contents / "x86_64-linux"))
    return contents.parent


def render(args, piece, destination, bundle):
    environment = os.environ.copy()
    if args.components_lib:
        components = str(Path(args.components_lib).resolve())
        # Ahead of anything the caller already had, so an installed copy
        # cannot shadow the checkout that was asked for.
        environment["PYTHONPATH"] = os.pathsep.join(
            [components] + [p for p in (environment.get("PYTHONPATH"),) if p])
        # PYTHONPATH decides which packages the render *imports*; this decides
        # which ones the composer reads metadata and patch 0 out of. They have
        # to be the same tree or the render runs one library's DSP with the
        # other's patches.
        environment["MPVST_COMPONENTS_LIB"] = components
    environment.setdefault("MPVST_BUNDLE", str(bundle))
    result = subprocess.run(
        [args.python, str(Path(args.mpvst).joinpath(*PREVIEW)),
         "--piece", piece, str(destination)],
        cwd=str(Path(args.mpvst)), env=environment, capture_output=True,
        check=False)
    report = result.stdout.decode("utf-8", "replace")
    errors = result.stderr.decode("utf-8", "replace")
    if result.returncode:
        # Not fatal to the run. A piece whose rack no longer matches the
        # component API says nothing about the others, and a gate that stopped
        # at the first one reported nothing at all about the rest - which is
        # how this arrived: `render failed: AureliaOverture` and seven pieces
        # unmeasured. The whole output still goes to stderr, so nothing is lost.
        sys.stderr.write(report)
        sys.stderr.write(errors)
        return Unrendered(reason(errors))
    data = destination.read_bytes()
    return normalize(report), hashlib.sha256(data).hexdigest(), len(data)


class Unrendered(object):
    """A piece the renderer could not produce at all, and why."""

    def __init__(self, reason):
        self.reason = reason


def reason(errors):
    """The one line of a traceback worth printing beside the piece name."""
    lines = [line for line in errors.splitlines() if line.strip()]
    return lines[-1] if lines else "render failed with no output"


def each_piece(args):
    with tempfile.TemporaryDirectory() as scratch:
        bundle = adapter_bundle(args.mpvst, scratch)
        for piece in args.pieces:
            destination = Path(scratch) / ("%s.wav" % piece)
            # (piece, Unrendered) or (piece, (report, digest, size)).
            yield piece, render(args, piece, destination, bundle)


def capture(args):
    # Merge, never replace. `--pieces` narrows what gets rendered, and a
    # capture that then wrote only those would quietly drop every piece it
    # was not asked about - which is exactly when you would reach for it, to
    # re-baseline one piece whose music deliberately changed.
    fixture = {
        "oracle": "mpvst examples/soundtrack/composer/preview.py",
        "pieces": {},
    }
    if GOLDEN.exists():
        fixture = json.loads(GOLDEN.read_text())
        fixture.setdefault("pieces", {})
    unrendered = []
    for piece, outcome in each_piece(args):
        if isinstance(outcome, Unrendered):
            # Its old entry stays: a capture that could not render a piece has
            # learned nothing about it, and dropping the entry would read as
            # "this piece is new" forever after.
            print("failed   %-18s %s" % (piece, outcome.reason))
            unrendered.append(piece)
            continue
        report, digest, size = outcome
        fixture["pieces"][piece] = {
            "wav_sha256": digest,
            "wav_bytes": size,
            "levels": levels(report),
            "report": report,
        }
        print("captured %-18s %s  %d bytes" % (piece, digest[:16], size))
    GOLDEN.parent.mkdir(exist_ok=True)
    GOLDEN.write_text(json.dumps(fixture, indent=2, sort_keys=True) + "\n")
    print("wrote %s" % GOLDEN)
    if unrendered:
        raise SystemExit("not captured: %s" % ", ".join(unrendered))


def verify(args):
    if not GOLDEN.exists():
        raise SystemExit("nothing captured yet: run with --capture")
    fixture = json.loads(GOLDEN.read_text())
    failures = []
    for piece, outcome in each_piece(args):
        if isinstance(outcome, Unrendered):
            print("failed   %-18s %s" % (piece, outcome.reason))
            failures.append("%s: did not render - %s" % (piece,
                                                         outcome.reason))
            continue
        report, digest, size = outcome
        record = fixture["pieces"].get(piece)
        if record is None:
            # A piece the golden has never seen. Not a failure: this fixture
            # detects *movement* in what it captured, and a piece added since
            # cannot have moved. Said out loud rather than skipped silently,
            # because a golden that lost an entry looks the same from here.
            print("new      %-18s not in the golden; nothing to compare"
                  % piece)
            continue
        if digest == record["wav_sha256"]:
            print("ok       %-18s identical (%s)" % (piece, digest[:16]))
            continue
        print("moved    %-18s %s != %s"
              % (piece, digest[:16], record["wav_sha256"][:16]))
        now = levels(report)
        before = record["levels"]
        worst = 0.0
        for name, value in sorted(before.items()):
            if name not in now:
                failures.append("%s: %s is gone from the report"
                                % (piece, name))
                continue
            drift = abs(now[name] - value)
            if name.endswith("_peak"):
                # These two are linear, not dB - scale the comparison so one
                # tolerance means the same thing everywhere.
                drift = abs(dbfs(now[name]) - dbfs(value))
            worst = max(worst, drift)
            if drift > args.tolerance_db:
                failures.append("%s: %s moved %.2f dB (%.3f -> %.3f)"
                                % (piece, name, drift, value, now[name]))
        print("         worst level shift %.2f dB (tolerance %.2f)"
              % (worst, args.tolerance_db))
        for line in difflib.unified_diff(
                record["report"].splitlines(), report.splitlines(),
                "captured", "now", lineterm="", n=1):
            print("         %s" % line)
    if failures:
        print()
        for line in failures:
            print("  %s" % line)
        raise SystemExit(1)


def dbfs(linear):
    from math import log10
    return 20.0 * log10(max(abs(linear), 1e-9))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--verify", action="store_true")
    # --vst3 is the name this had while the checkout was micropython-vst3;
    # it still answers, so an older invocation keeps working.
    parser.add_argument("--mpvst", "--vst3", dest="mpvst",
                        default=str(DEFAULT_MPVST),
                        help="an mpvst checkout (default: %s)" % DEFAULT_MPVST)
    parser.add_argument("--python", default=sys.executable,
                        help="an interpreter with numpy, pydevices-audioif and "
                             "the component packages (see the module docstring)")
    parser.add_argument("--components-lib", default=None,
                        help="an audiocomponents checkout's lib/ directory, put "
                             "on the render's PYTHONPATH instead of installing "
                             "audioinstruments and audioeffects there")
    parser.add_argument("--pieces", default=None,
                        help="comma-separated subset")
    parser.add_argument("--tolerance-db", type=float, default=1.0,
                        help="how far a level may move once the WAV has")
    args = parser.parse_args()
    if args.capture == args.verify:
        raise SystemExit("choose exactly one of --capture / --verify")
    args.pieces = ([name.strip() for name in args.pieces.split(",")
                    if name.strip()] if args.pieces else pieces(args.mpvst))
    print("pieces: %s\n" % ", ".join(args.pieces))
    if args.capture:
        capture(args)
    else:
        verify(args)


main()
