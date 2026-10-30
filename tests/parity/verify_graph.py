#!/usr/bin/env python3
"""A graph renders the same samples on every interpreter.

    verify_graph.py --micropython PATH [--circuitpython PATH]
    verify_graph.py --micropython PATH --fault lsb     # must FAIL
    verify_graph.py --micropython PATH --fault late    # must FAIL
    verify_graph.py --micropython PATH --fault phaser  # must FAIL (and lag, lfo, hold)
    verify_graph.py --micropython PATH --fault twopole # must FAIL (and overdrive, pan)
    verify_graph.py --micropython PATH --fault drain   # must FAIL (and split)

Runs two probes on this CPython (``audiograph`` from the installed wheel) and
on each interpreter named, and compares the renders sample for sample.
``graph_parity_probe.py`` is the runner: every output sample of every block,
the buffers' peaks and a mono graph's blocks. ``graph_nodes_probe.py`` is the
phaser, the glide, the LFO's shapes, the sample-and-hold, the two-pole filters,
the overdrive and the pan: every sample each of them writes. ``graph_source_probe.py`` is the graph pulled as a source,
through its queue, which must render what ``run()`` renders. The block headers (which voices ran, which were still
sounding), the parameters read back and the probes' own checks must match
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

``--fault`` plants a mistake on every interpreter after the first, in the
probe it belongs to (each probe's docstring has what its faults are), and the
run must then fail. It is how this gate shows it can.
"""

import argparse
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
#: Each probe and the faults it can plant.
PROBES = (
    ("runner", HERE / "graph_parity_probe.py", ("lsb", "late")),
    ("nodes", HERE / "graph_nodes_probe.py", ("phaser", "lag", "lfo", "hold", "twopole", "overdrive", "pan")),
    ("source", HERE / "graph_source_probe.py", ("drain", "split")),
)
FAULTS = tuple(f for _, _, faults in PROBES for f in faults)
#: Lines whose fields after the tag are samples, compared within the tolerance.
SAMPLE_LINES = ("pcm", "peaks", "mono", "node")
#: Leading fields of a sample line that are not samples, compared exactly: a
#: pcm line's block, enable mask and sounding mask; a node line's block and
#: node name.
HEADER_FIELDS = {"pcm": 3, "peaks": 0, "mono": 0, "node": 2}


def render(argv, probe, fault, timeout):
    command = argv + [str(probe)] + (["--fault", fault] if fault else [])
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
    parser.add_argument("--fault", choices=FAULTS,
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

    failed = False
    reference = interpreters[0][0]
    for probe_name, probe, faults in PROBES:
        renders = {}
        for position, (name, argv) in enumerate(interpreters):
            fault = args.fault if position and args.fault in faults else None
            code, lines = render(argv, probe, fault, args.timeout)
            renders[name] = lines
            if code:
                print("FAIL  %-14s %s: the probe failed its own checks (exit %d)"
                      % (name, probe_name, code))
                for line in lines[-6:]:
                    print("        " + line[:120])
                failed = True

        for name, _ in interpreters[1:]:
            compared, differing, worst, first, mismatches = compare(
                renders[reference], renders[name], args.tolerance)
            verdict = "FAIL" if mismatches else "ok  "
            print("%s  %-6s %s = %s: %d samples, %d differ, worst %g counts "
                  "(tolerance %g)" % (verdict, probe_name, reference, name,
                                      compared, differing, worst, args.tolerance))
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
