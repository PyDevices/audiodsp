#!/usr/bin/env python3
"""Every target must render these nodes identically. That is the whole gate.

    verify_dsp.py --micropython PATH [--circuitpython PATH]

`docs/correctness-standard.md` is what this implements. There is no stored
digest and no oracle: the probes are run on every interpreter given, and the
gate is that their output is byte-identical. A disagreement is the finding.

**Two interpreters are the minimum, and fewer is refused rather than passed.**
A single-interpreter run has nothing to compare and cannot fail, which is worse
than no gate at all because it reports green. This script used to accept
`--interpreters cpython` and used to print `skipping micropython (not built
at ...)` and carry on; both are gone.

## Why agreement is the right check for these nodes

All three targets compile the same C - `shared/audioif_dynamics.c`,
`audioif_splitter.c`, `audioif_midside.c`, `audioif_multiply.c`,
`audioif_suboctave.c`, `audioif_feedback_delay.c`, `audioif_shaper.c`,
`audioif_samplehold.c`, `audioif_ladder.c`, `audioif_convolve.c`,
`audioif_tank.c`,
`audioif_flanger.c`, `audioif_granular_pitch_shift.c`, with `audioif_fft.c` and
`audioif_trig.c` under the convolver. The CPython extension links it, the
MicroPython usermod compiles it, and the patched CircuitPython build compiles it
again. So a difference between two of them is never a difference of intent: it
is a width, an undefined shift, a compiler's choice or an architecture. That is
exactly the class of defect a stored digest cannot see, and it is not
hypothetical - on 2026-09-09 the MicroPython flanger overflowed `int32_t` in its
wet interpolation on full-scale material and every stored fixture matched to the
byte throughout.

**What agreement cannot see, said plainly:** a change to the shared C moves all
three together and stays green. This gate sees divergence between targets, not
drift over time. Drift is the traits' job -
`tests/test_cpython_<module>.py`, one file per module, each opening with its
trait table and bars.

## Five of these are unusually sensitive, which is most of the reason to run
## them everywhere

The delay's loop is recursive, so a one-ulp disagreement between two builds
would not stay one ulp; the waveshaper's half-bands are all-pass recursions
running at up to eight times the sample rate; the ladder's loop is recursive AND
solved, so a difference has the solver's seed to grow through as well, and
several of its fixtures sit where the loop sustains a tone of its own and
nothing damps a difference at all; every convolver output sample is a sum of
hundreds of float products through two transforms; and the tank is ten
recirculating lines feeding each other in float, which is the delay's problem
again with the loop closed twice over.

## Why some nodes have two probes rather than one appended case

One comparison covers a probe's whole output, so a case appended to
`feedback_delay_probe.py` would move the very numbers that say `wow_shape`,
`delay_slew`, `wow_am_depth` and `loop_semitones` changed nothing. Those live in
`feedback_delay_options_probe.py`, whose first two cases render exactly what
that file's `plain` renders. `dynamics_extras_probe.py` and
`dynamics_options_probe.py` carry the same additivity check: the first case of
each sets none of the options it exists to cover, and its numbers are a case of
`dynamics_probe.py` line for line.

`filter_f32_probe.py` also prints invariants rather than only PCM - the block at
which a tail reaches exact zero, and the depth of a null at zero feedback -
because those two are why `audiobiquad` was added, and PCM alone would not say
whether either still held.
"""

import argparse
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WORKSPACE = ROOT.parent

#: (probe, the module the port provides it as, {interpreter: why it is skipped
#:  there}, pending). A skip is a stated exception for an interpreter that
#: cannot run that probe for a named reason - never a missing binary.
#:
#: `pending` is the third state, and it exists so an uncoverable probe is
#: *visible* rather than either silently passing or standing red forever. It
#: must name an issue. A pending probe is not run and is not counted as a
#: comparison; the summary prints how many there are, because that number
#: going up is a regression in coverage even when nothing is failing.
PROBES = (
    ("dynamics_probe.py", "audiodynamics", {}, None),
    ("route_probe.py", "audioroute", {}, None),
    # The stated skip this used to carry -- "its coverage variant does not
    # compile audiospeed" -- was true of the 10.2.1 oracle and stopped being
    # true when the 10.3.0 build gained the module. It ran three ways from the
    # day the skip went stale, so it is unskipped rather than reworded.
    ("route_dry_probe.py", "audioroute", {}, None),
    ("midside_probe.py", "audioroute", {}, None),
    ("multiply_probe.py", "audiomath", {}, None),
    ("suboctave_probe.py", "audiomath", {}, None),
    ("feedback_delay_probe.py", "audioecho", {}, None),
    ("feedback_delay_options_probe.py", "audioecho", {}, None),
    ("ladder_probe.py", "audioladder", {}, None),
    ("dynamics_extras_probe.py", "audiodynamics", {}, None),
    ("dynamics_options_probe.py", "audiodynamics", {}, None),
    ("waveshaper_probe.py", "audioshaper", {}, None),
    # A probe of its own rather than cases appended above: one comparison
    # covers a probe's whole output, so a `SampleHold` case added to
    # `waveshaper_probe.py` would move the very numbers that say the
    # Waveshaper's bytes did not change. Its last lines are counts rather than
    # PCM -- refreshes over a whole number of periods -- because the claim the
    # node exists for is exactness over time, and PCM does not say whether that
    # still holds (audioif#97).
    ("samplehold_probe.py", "audioshaper", {}, None),
    ("convolve_probe.py", "audioconvolve", {}, None),
    ("filter_f32_probe.py", "audiobiquad", {}, None),
    ("modal_probe.py", "audiomodal", {}, None),
    # synthio.Biquad and audiofilters.Filter are CircuitPython's, so they are
    # held to CircuitPython's bytes and NOT to a stored digest -- which is the
    # whole of audioif#77: graded against its own capture, this probe reported
    # green for months while the CPython twin ran audioif's widened fixed point
    # and MicroPython ran CircuitPython's Q15, 11 LSB apart by the eighth
    # sample of an 800 Hz low-pass. Comparing the interpreters is what sees it.
    ("biquad_component_probe.py", "audiofilters",
     {"circuitpython": "two named departures from the pinned 10.3.0 build, "
                       "both from PRs upstream has already merged and not yet "
                       "released: PEAKING_EQ's b2 sign (8fabdbbfb1) and the "
                       "half-cleared filter reset (8a3deace5c). This probe "
                       "also covers Note.filter cascades, which are this "
                       "port's extension and which 10.3.0 refuses. See "
                       "docs/upstream-diff.md"},
     None),
    # audiomixer is CircuitPython's, so like biquad_component_probe.py it is
    # held to the interpreters agreeing rather than to a capture. The twin
    # scaled a voice in float64 where the kernel scales in float32 and
    # truncated a different integer on 56 of the 65536 int16 values at level
    # 100/127 -- invisible to every stored digest in the repository, because
    # none of their material lands on one. audioif#84.
    ("mixer_level_precision_probe.py", "audiomixer", {}, None),
    # A voice mixes from its source's buffer as that buffer stands at mix
    # time, because what it holds is a pointer into it. The twin copied the
    # block at play() instead, so a class that settles a filter behind a voice
    # it has already attached -- Saturation's coupling pole -- rendered its
    # first block differently on CPython than on every native build.
    # audioif#89.
    ("mixer_borrowed_block_probe.py", "audiomixer", {}, None),
    ("tank_probe.py", "audioverb", {}, None),
    ("flanger_probe.py", "audiodelays",
     {"circuitpython": "upstream's own Flanger overflows int32 in its wet "
                       "interpolation on full-scale material and ours does "
                       "not - audioif#76, a deliberate departure recorded in "
                       "docs/upstream-diff.md"},
     None),
    ("granular_pitch_shift_probe.py", "audiodelays", {}, None),
    ("resampler_probe.py", "audiospeed", {}, None),
    # Two departures from 10.3.0, both of them upstream bugs this port
    # declines to reproduce: the Q16 rate truncates there and rounds here
    # (audioif#92), and the phase accumulator is zeroed at every source buffer
    # there and carried here (audioif#91). See docs/upstream-diff.md.
    #
    # `resampler_probe.py` above cannot see either one -- it asks only for 2.0,
    # 1.0 and 0.5, which are exact in Q16 and divide a buffer exactly -- which
    # is why this probe exists rather than a case appended to that one.
    #
    # The MicroPython leg needs a build carrying the fix. Any binary from
    # before it renders CircuitPython's bytes here, to the byte, because the
    # port was faithful; a stale interpreter therefore fails this line for a
    # reason that is not a defect.
    ("speedchanger_hold_probe.py", "audiospeed",
     {"circuitpython": "upstream's SpeedChanger truncates its Q16 rate and "
                       "restarts its phase at every source buffer, and ours "
                       "does neither - audioif#92 and audioif#91, both "
                       "deliberate departures recorded in "
                       "docs/upstream-diff.md"},
     None),
    ("echo_filter_probe.py", "audiodelays", {}, None),
    ("freeverb_filter_probe.py", "audiofreeverb", {}, None),
)


def run_probe(argv_prefix, probe, module):
    """A probe's stdout, newline-normalised. Raises if it does not run."""
    environment = os.environ.copy()
    # CPython imports audioif from the installed package. MicroPython and
    # CircuitPython take these modules from their own firmware, so MICROPYPATH
    # is only here for anything a probe loads out of the tree -- `lib/` is on
    # it because `audioif_util` lives there, and a probe that derives a setting
    # in Python has to round it the way a board would before handing it over
    # (docs/correctness-standard.md, audioif#80).
    environment["MICROPYPATH"] = "%s:%s" % (ROOT, ROOT / "lib")
    # The same directory for the CPython leg, ahead of whatever is installed.
    # It is pure Python with no DSP in it, and putting it here is what lets
    # this gate run from a checkout whose installed wheel predates the module
    # -- which is every checkout, the first time.
    environment["PYTHONPATH"] = os.pathsep.join(
        [str(ROOT / "lib")] + ([environment["PYTHONPATH"]]
                               if environment.get("PYTHONPATH") else []))
    result = subprocess.run(
        argv_prefix + [str(HERE / probe), module],
        cwd=str(ROOT), env=environment, capture_output=True, check=False)
    if result.returncode:
        sys.stderr.buffer.write(result.stdout)
        sys.stderr.buffer.write(result.stderr)
        raise SystemExit("probe failed: %s %s" % (probe, module))
    return result.stdout.replace(b"\r\n", b"\n")


def first_difference(left, right):
    for index in range(min(len(left), len(right))):
        if left[index] != right[index]:
            return index
    if len(left) != len(right):
        return min(len(left), len(right))
    return None


def interpreter_table(args):
    """`{name: argv prefix}`. A named interpreter that is not built is an
    error, not a skip: the whole point is the comparison."""
    found = {"cpython": [sys.executable]}
    for name, path in (("micropython", args.micropython),
                       ("circuitpython", args.circuitpython)):
        if not path:
            continue
        if not Path(path).exists():
            raise SystemExit("%s was named but is not built at %s"
                             % (name, path))
        found[name] = [str(path)]
    return found


def verify(args):
    interpreters = interpreter_table(args)
    if len(interpreters) < 2:
        raise SystemExit(
            "this gate compares interpreters against each other, so it needs "
            "at least two. Pass --micropython PATH (and --circuitpython PATH "
            "where the build has these modules). One interpreter cannot "
            "disagree with itself, and a run that cannot fail is worse than "
            "no run: it reports green.")
    print("interpreters: %s\n" % ", ".join(sorted(interpreters)))

    failures = []
    compared = 0
    pending = []
    agreements = []
    for probe, module, skips, blocked_by in PROBES:
        if blocked_by:
            pending.append((probe, blocked_by))
            print("PENDING  %-30s %s" % (probe, blocked_by))
            continue
        names = [name for name in sorted(interpreters) if name not in skips]
        for name in sorted(skips):
            if name in interpreters:
                print("skipping %-30s %-14s (%s)"
                      % (probe, name, skips[name]))
        if len(names) < 2:
            failures.append("%s: fewer than two interpreters left after its "
                            "stated skips, so nothing was compared" % probe)
            continue

        rendered = {}
        for name in names:
            rendered[name] = run_probe(interpreters[name], probe, module)

        reference = names[0]
        agreed = True
        for name in names[1:]:
            compared += 1
            if rendered[name] == rendered[reference]:
                continue
            agreed = False
            offset = first_difference(rendered[reference], rendered[name])
            print("FAIL     %-30s %s and %s differ at output byte %s"
                  % (probe, reference, name, offset))
            print("             %-14s %d bytes" % (reference,
                                                   len(rendered[reference])))
            print("             %-14s %d bytes" % (name, len(rendered[name])))
            failures.append("%s: %s and %s" % (probe, reference, name))
        if agreed:
            agreements.append(probe)
            print("ok       %-30s %s agree (%d bytes)"
                  % (probe, " = ".join(names), len(rendered[reference])))

    print("\n%d comparisons, %d failures, %d pending"
          % (compared, len(failures), len(pending)))
    for probe, blocked_by in pending:
        print("  pending  %-30s %s" % (probe, blocked_by))

    # --known-divergent narrows the gate to the divergences we have already
    # filed, WITHOUT blinding it. Three things still fail the run:
    #   * a probe that diverges and is not on the list      (a new defect)
    #   * a probe on the list that now agrees               (the list is stale)
    #   * anything that was never about agreement at all    (build, smoke)
    # That last property is the point. A blanket continue-on-error would report
    # green for all three, and this file's own argument is that a gate which
    # cannot fail is worse than no gate.
    expected = set(args.known_divergent or [])
    if expected:
        diverged = {line.split(":", 1)[0] for line in failures}
        unexpected = sorted(diverged - expected)
        stale = sorted(expected & set(agreements))

        for probe in sorted(diverged & expected):
            print("  XFAIL    %-30s diverges as expected" % probe)
        for probe in stale:
            print("  UNEXPECTED PASS  %-22s agrees now -- drop it from "
                  "--known-divergent" % probe)

        if not unexpected and not stale:
            print("\n%d expected divergence(s), none new. Gate satisfied."
                  % len(diverged & expected))
            return
        failures = ["%s: diverged and is not expected" % p for p in unexpected]
        failures += ["%s: no longer diverges" % p for p in stale]

    if failures:
        for line in failures:
            print("  %s" % line)
        raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--micropython", default=None,
                        help="a MicroPython binary with the audioif usermod")
    parser.add_argument("--circuitpython", default=None,
                        help="a patched CircuitPython build")
    parser.add_argument("--known-divergent", action="append", metavar="PROBE",
                        help="a probe whose divergence is already filed; the "
                             "run still fails on any OTHER divergence, and "
                             "fails if this probe starts agreeing")
    verify(parser.parse_args())


main()
