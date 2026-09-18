#!/usr/bin/env python3
"""Verify biquad PCM across every FilterMode against committed metadata.

Unlike the other verifies here, the golden is captured from *this* port and
not from CircuitPython: three deliberate deviations live in this path. See
docs/upstream-diff.md. Pass --capture to re-record after an intended change.
"""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
fixture_path = Path(__file__).parent / "golden" / "biquad_component.json"
probe = Path(__file__).parent / "biquad_component_probe.py"
environment = os.environ.copy()
# The probe rounds Q and A the way a board would (audioif_util.float32), so
# `lib/` goes on the path ahead of whatever wheel is installed -- same reason
# as verify_dsp.py, and it is what lets this run from a fresh checkout.
environment["PYTHONPATH"] = os.pathsep.join(
    [str(ROOT / "lib")] + ([environment["PYTHONPATH"]]
                           if environment.get("PYTHONPATH") else []))
result = subprocess.run(
    [sys.executable, str(probe)],
    cwd=ROOT, env=environment, capture_output=True, check=False,
)
if result.returncode:
    sys.stderr.buffer.write(result.stdout)
    sys.stderr.buffer.write(result.stderr)
    raise SystemExit(result.returncode)
actual = hashlib.sha256(result.stdout.replace(b"\r\n", b"\n")).hexdigest()

if "--capture" in sys.argv:
    fixture_path.write_text(json.dumps(
        {"cpython_stdout_sha256": actual}, indent=2) + "\n")
    print("captured", actual)
    raise SystemExit(0)

expected = json.loads(fixture_path.read_text())["cpython_stdout_sha256"]
if actual != expected:
    raise SystemExit(
        "biquad PCM does not match committed metadata: "
        f"expected {expected}, got {actual}"
    )
print("CPython biquad parity matches", actual)
