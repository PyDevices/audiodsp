#!/usr/bin/env python3
"""A graph renders the same samples on every interpreter.

    verify_graph.py --micropython PATH [--circuitpython PATH]
    verify_graph.py --micropython PATH --fault lsb     # must FAIL
    verify_graph.py --micropython PATH --fault late    # must FAIL

Runs ``graph_parity_probe.py`` on this CPython (``audiograph`` from the
installed wheel) and on each interpreter named, and compares the renders
sample for sample: every output sample of every block, the buffers' peaks and
a mono graph's blocks. The block headers (which voices ran, which were still
sounding), the parameters read back and the probe's own checks must match
exactly.

**The tolerance is 0 counts.** All three compile the same
``src/shared/audiodsp_graph.c`` over the same kernels, and the probe hands
every interpreter the same floats, so there is no difference of intent for a
tolerance to absorb: a sample that differs by one count is a binding that
passed something differently, a width, or a compiler's choice, and that is
the finding. ``--tolerance`` exists to measure a divergence, not to accept
one.

Two interpreters are the minimum, and fewer is refused: one cannot disagree
with itself. CI runs CPython against MicroPython; the three-way with the
CircuitPython unix build is a local run, as for verify_dsp.py.

``--fault`` plants a mistake on every interpreter after the first (the probe's
docstring has what each one is), and the run must then fail. It is how this
gate shows it can.
"""

import argparse
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROBE = HERE / "graph_parity_probe.py"
#: Lines whose fields after the tag are samples, compared within the tolerance.
SAMPLE_LINES = ("pcm", "peaks", "mono")
#: Leading fields of a sample line that are not samples: a pcm line's block,
#: enable mask and sounding mask, compared exactly.
HEADER_FIELDS = {"pcm": 3, "peaks": 0, "mono": 0}


def render(argv, fault, timeout):
    command = argv + [str(PROBE)] + (["--fault", fault] if fault else [])
    environment = os.environ.copy()
    environment["MICROPYPATH"] = "%s:%s" % (ROOT, ROOT / "lib")
    result = subprocess.run(command, cwd=str(ROOT), env=environment,
                            capture_output=True, check=False, timeout=timeout)
    text = result.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
    if "GRAPH PARITY SKIPPED" in text:
        raise SystemExit("%s has no audiograph" % argv[0])
    return result.returncode, text.splitlines()


def compare(reference, other, tolerance):
    """(samples compared, samples differing, worst delta, first difference,
    structural mismatches)."""
    compared = differing = worst = 0
    first = None
    mismatches = []
    if len(reference) != len(other):
        mismatches.append("%d lines against %d" % (len(reference), len(other)))
    for number, (one, two) in enumerate(zip(reference, other)):
        a, b = one.split(), two.split()
        tag = a[0] if a else ""
        if tag not in SAMPLE_LINES or not b or b[0] != tag:
            if one != two:
                mismatches.append("line %d: %r against %r"
                                  % (number + 1, one[:80], two[:80]))
            continue
        head = 1 + HEADER_FIELDS[tag]
        if a[:head] != b[:head] or len(a) != len(b):
            mismatches.append("line %d: %r against %r"
                              % (number + 1, " ".join(a[:head]),
                                 " ".join(b[:head])))
            continue
        for index in range(head, len(a)):
            delta = abs(float(a[index]) - float(b[index]))
            compared += 1
            if delta:
                differing += 1
                if first is None:
                    first = "%s, field %d" % (" ".join(a[:head]), index - head)
                worst = max(worst, delta)
    if worst > tolerance:
        mismatches.append("a sample differs by %g counts, over the tolerance "
                          "of %g" % (worst, tolerance))
    return compared, differing, worst, first, mismatches


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--micropython", help="a MicroPython with audiograph")
    parser.add_argument("--circuitpython", help="a CircuitPython with audiograph")
    parser.add_argument("--tolerance", type=float, default=0.0,
                        help="counts a sample may differ by (default 0)")
    parser.add_argument("--fault", choices=("lsb", "late"),
                        help="plant a mistake on every interpreter but the first")
    parser.add_argument("--timeout", type=float, default=300.0,
                        help="seconds one render may take")
    args = parser.parse_args()

    interpreters = [("cpython", [sys.executable])]
    for name, path in (("micropython", args.micropython),
                       ("circuitpython", args.circuitpython)):
        if path:
            if not Path(path).exists():
                raise SystemExit("%s was named but is not built at %s" % (name, path))
            interpreters.append((name, [str(path)]))
    if len(interpreters) < 2:
        raise SystemExit("this gate compares interpreters, so it needs at least "
                         "two: pass --micropython PATH (and --circuitpython PATH)")

    renders = {}
    failed = False
    for position, (name, argv) in enumerate(interpreters):
        fault = args.fault if position else None
        code, lines = render(argv, fault, args.timeout)
        renders[name] = lines
        if code:
            print("FAIL  %-14s the probe failed its own checks (exit %d)" % (name, code))
            for line in lines[-6:]:
                print("        " + line[:120])
            failed = True

    reference = interpreters[0][0]
    for name, _ in interpreters[1:]:
        compared, differing, worst, first, mismatches = compare(
            renders[reference], renders[name], args.tolerance)
        verdict = "FAIL" if mismatches else "ok  "
        print("%s  %s = %s: %d samples, %d differ, worst %g counts (tolerance %g)"
              % (verdict, reference, name, compared, differing, worst, args.tolerance))
        if first:
            print("        first difference at %s" % first)
        for line in mismatches[:5]:
            print("        " + line)
        failed = failed or bool(mismatches)

    if failed:
        print("GRAPH RENDERS DIFFER" + (" (fault planted: %s)" % args.fault
                                        if args.fault else ""))
        raise SystemExit(1)
    print("GRAPH RENDERS AGREE: %s" % " = ".join(name for name, _ in interpreters))


main()
