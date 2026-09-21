#!/usr/bin/env python3
"""The one gate that can see a synthio voice-ceiling change (audiodsp#27).

WHAT THIS COVERS THAT THE OTHER FOUR CANNOT
===========================================

`verify_acceptance`, `verify_effects`, `verify_streaming` and
`verify_biquad` all render material that stays inside the mix-down
limiter's linear region, so every one of them is byte-identical at any
value of the voice ceiling. A ceiling change ships green through all
four. `mixdown_knee_probe.py`'s material crosses the +/-28000 knee, where
`SYNTHIO_MIX_DOWN_SCALE` -- a function of the ceiling -- multiplies every
sample, so its output moves when the ceiling moves.

Scope, stated narrowly on purpose: the CPython target reads exactly ONE
of the ceiling's five sites, `src/cpython/synthio.py`'s
`Synthesizer.max_polyphony`, which it hands to `_audiodsp.mixdown_i32` as
the limiter divisor. That is the only site this gate observes. The
header, `micropython.mk` and `micropython.cmake` copies are read by no CI
gate at all, so `tests/test_voice_ceiling_consistency.py` is still the
guard against a half-applied change. What this gate adds is the other
failure: a ceiling moved CONSISTENTLY across all five sites, which that
test passes by construction and which silently changes the audio.

WHAT IS ENFORCED AGAINST WHAT -- read this before trusting the word
"oracle" anywhere near this file
================================================================

Check (a), the gate proper: the probe's full stdout must equal
`stdout` in the golden. `stdout` is THIS PORT's output, not the
CircuitPython oracle's, and above the knee the two cannot agree. The
port ships CIRCUITPY_SYNTHIO_MAX_CHANNELS=64 and the pinned oracle is
built at 14; SYNTHIO_MIX_DOWN_SCALE is 129 at 64 and 623 at 14, and it
scales every sample past the knee. So no material can be both above the
knee and oracle-identical. The above-knee half of this gate is therefore
NOT oracle-enforced, and nothing here should be described as if it were.
That divergence is the ceiling and nothing else, and it is written
down: docs/upstream-diff.md, "The ceiling later moved 14 -> 64, which
gives up that alignment above the knee".

Check (b), the anti-launder tripwire, and the only check here that
survives a re-capture: the ceiling this interpreter actually reports
(`synthio.Synthesizer.max_polyphony`) must equal `port_max_polyphony` in
the golden. Re-capture `stdout` under a changed ceiling and check (a)
goes green again -- but (b) goes red, because the golden still declares
the old number. Getting green then requires editing `port_max_polyphony`
too, which puts the ceiling change on one visible, reviewable line of the
diff instead of hiding it inside ten changed digests. This was measured,
not assumed: with `src/cpython/synthio.py`'s ceiling set to 24 and
`stdout` re-captured, (a) passed and (b) failed.

Check (c), narrow and worth having anyway: the below_knee lines of the
actual output must equal the below_knee lines of `circuitpython_stdout`,
the oracle's own captured answer. Below the knee the port and the oracle
ARE byte-identical (measured on all three runtimes, 2026-09-06), so while
(a) passes this cannot fail -- it is logically implied by (a). It does
NOT catch a laundered ceiling change, because a ceiling change leaves the
below-knee lines untouched; that is check (b)'s job and it is why (b)
exists. What (c) catches is a re-capture of `stdout` after some OTHER
change that moved below-knee output -- an oscillator or envelope edit --
where the oracle field was left behind.

REPRODUCING THE PLANTED FAULT (there is a trap here)
====================================================

Edit `max_polyphony = 64` to some other two-digit number in
`src/cpython/synthio.py` and run this gate; it must exit non-zero. Then
`git checkout -- src/cpython/synthio.py` and run it again; it must be
green. **Delete `src/cpython/__pycache__` between those steps.** `= 64`
and `= 24` are the same byte length, and CPython validates a cached .pyc
on (source mtime, source size) -- so a plant and a revert inside the same
second leave a stale .pyc that passes validation, and the gate then
reports the planted fault's numbers against a source that says 64. That
looked exactly like a broken revert here on 2026-09-06 and was not one.
Check `python -c "import synthio; print(synthio.Synthesizer.max_polyphony)"`
after each step rather than trusting the file.

CAPTURING
=========

`--capture` re-records `stdout` and `sha256` from this interpreter.
`--capture-reference NAME BINARY` runs the probe under BINARY and records
`NAME_stdout` plus that binary's sha256. As with `verify_biquad.py` and
`verify_acceptance.py`: capturing after the fact, on a change whose gates
went red, is not the fix. The delta gets reported.
"""

import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
PARITY = Path(__file__).parent
PROBE = PARITY / "mixdown_knee_probe.py"
fixture_path = PARITY / "golden" / "mixdown_knee.json"

# The architecture `stdout` was captured on. Elsewhere the gate is exact
# too, but against that architecture's own accepted baseline -- recorded
# only after a human has read a deviation report (Brad, 2026-09-02;
# verify_effects.py carries the same rule and the precedent).
REFERENCE_MACHINE = "x86_64"

BELOW = "below_knee"


def run(command):
    """The probe's stdout, CRLF-normalised, or a hard exit."""
    result = subprocess.run(
        command, cwd=ROOT, env=os.environ.copy(),
        capture_output=True, check=False,
    )
    if result.returncode:
        sys.stderr.buffer.write(result.stdout)
        sys.stderr.buffer.write(result.stderr)
        raise SystemExit(result.returncode)
    # Windows CPython writes CRLF from the probe subprocess, so normalise
    # before hashing or every Windows job fails on a hash while the PCM is
    # byte-identical. verify_streaming.py and verify_effects.py do the same.
    return result.stdout.replace(b"\r\n", b"\n")


def below_knee(lines):
    return [line for line in lines if line.startswith(BELOW + " ")]


fixture = json.loads(fixture_path.read_text())

if "--capture-reference" in sys.argv:
    index = sys.argv.index("--capture-reference")
    name, binary = sys.argv[index + 1], sys.argv[index + 2]
    binary_path = Path(binary).resolve()
    output = run([str(binary_path), str(PROBE)])
    fixture[f"{name}_stdout"] = output.decode().splitlines()
    fixture[f"{name}_binary_sha256"] = hashlib.sha256(
        binary_path.read_bytes()).hexdigest()
    fixture_path.write_text(
        json.dumps(fixture, indent=1, sort_keys=True) + "\n")
    print(f"captured {name}_stdout from {binary_path}")
    raise SystemExit(0)

actual_bytes = run([sys.executable, str(PROBE)])
actual = actual_bytes.decode().splitlines()
digest = hashlib.sha256(actual_bytes).hexdigest()

if "--capture" in sys.argv:
    fixture["stdout"] = actual
    fixture["sha256"] = digest
    fixture_path.write_text(
        json.dumps(fixture, indent=1, sort_keys=True) + "\n")
    print("captured", digest)
    raise SystemExit(0)

machine = platform.machine()
expected = fixture["stdout"]
accepted = fixture.get("stdout_sha256_by_arch", {}).get(machine)

if actual != expected:
    if accepted and machine != REFERENCE_MACHINE and \
            digest == accepted["sha256"]:
        print(f"mix-down knee parity matches {machine}'s accepted baseline",
              digest)
        # Check (b) is skipped on an accepted per-architecture baseline: the
        # oracle capture is x86_64's and there is nothing to compare against
        # here. The anti-launder property holds on the reference
        # architecture, which is where `stdout` gets re-captured.
        raise SystemExit(0)
    report = [
        "mix-down knee PCM does not match this port's committed output.",
        f"  machine {machine} (reference architecture: "
        f"{REFERENCE_MACHINE})",
        f"  digest  {digest}",
        "",
        "  This gate exists to notice a change to the synthio voice "
        "ceiling -- src/cpython/synthio.py's max_polyphony, the divisor "
        "handed to _audiodsp.mixdown_i32. If you changed it, that is what "
        "you are looking at. If you did not, something moved the "
        "above-knee limiter arithmetic.",
        "",
        "  expected:",
    ]
    report += [f"    {line}" for line in expected]
    report.append("  actual:")
    report += [f"    {line}" for line in actual]
    if machine != REFERENCE_MACHINE and not accepted:
        report += [
            "",
            f"  {machine} has no accepted baseline recorded, so it was "
            "compared against the x86_64 capture. Cross-architecture "
            "bit-identity is not required (Brad, 2026-09-02). Accepting "
            "this architecture means adding an entry to "
            "stdout_sha256_by_arch with the hash, the date and the "
            "evidence -- a human judgement, not a formality. Do not add a "
            "tolerance and do not re-capture `stdout` on a non-reference "
            "runner.",
        ]
    raise SystemExit("\n".join(report))

# Check (b): the declared ceiling must match the ceiling this build
# actually has. This is the check a re-capture cannot silence -- see the
# module docstring.
ceiling = subprocess.run(
    [sys.executable, "-c",
     "import synthio; print(synthio.Synthesizer.max_polyphony)"],
    cwd=ROOT, env=os.environ.copy(), capture_output=True, check=False,
)
if ceiling.returncode or not ceiling.stdout.strip().isdigit():
    sys.stderr.buffer.write(ceiling.stdout)
    sys.stderr.buffer.write(ceiling.stderr)
    raise SystemExit(
        "could not read synthio.Synthesizer.max_polyphony from this build; "
        "the gate cannot confirm which voice ceiling produced the output "
        "above, so it is not reporting a pass.")
declared = fixture["port_max_polyphony"]
reported = int(ceiling.stdout.strip())
if reported != declared:
    raise SystemExit("\n".join([
        "the voice ceiling changed and the golden was not told.",
        "",
        f"  golden port_max_polyphony: {declared}",
        f"  this build reports:        {reported}",
        "",
        "  The PCM above matched `stdout`, so either the ceiling moved and "
        "`stdout` was re-captured with it -- the launder this check exists "
        "to catch -- or the golden is stale. Either way the fix is not to "
        "re-capture: state the new ceiling in "
        "tests/parity/golden/mixdown_knee.json (port_max_polyphony and "
        "port_mix_down_scale, which is 0x0fffffff / (32768 * N - 28000)) in "
        "the same commit that moves it, so the change is one readable line "
        "rather than ten changed digests.",
    ]))

# Check (c): the oracle's own below-knee bytes. See the module docstring
# for what this does and does not catch.
oracle_below = below_knee(fixture["circuitpython_stdout"])
actual_below = below_knee(actual)
if actual_below != oracle_below:
    raise SystemExit("\n".join([
        "the port field was re-captured but the oracle field was not.",
        "",
        "  `stdout` matches, so this port agrees with its own committed "
        "output -- but the below-knee lines no longer agree with "
        "`circuitpython_stdout`, the oracle's captured answer. Below the "
        "knee those two are byte-identical by measurement, so this can "
        "only happen if `stdout` was re-captured under changed "
        "arithmetic while the oracle field was left alone. That is the "
        "launder this check exists to catch; it is not an oracle parity "
        "failure and must not be reported as one.",
        "",
        "  oracle capture:",
    ] + [f"    {line}" for line in oracle_below] + [
        "  actual:",
    ] + [f"    {line}" for line in actual_below]))

print("mix-down knee parity matches", digest)
