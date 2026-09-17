"""A looping sample too short to fill one packed word must not hang (#85).

The mixer consumes whole 32-bit words, so a one-frame mono sample -- two
bytes -- measures zero words and a looping voice takes zero of them per pass
forever. On the native builds that is an unbounded spin; found when two Phase 4
effects primed their mixers with a one-frame silence and their MicroPython
renders hung for over 90 s.

**Run in a subprocess, under a timeout, on purpose.** What is being tested is
termination, and a test for termination that runs in-process cannot report the
failure it exists to catch: the job hangs instead, with no name on it. Twenty
seconds is roughly a hundred times what the whole probe needs.

The cases live in `tests/parity/mixer_short_loop_probe.py` rather than here,
because they have to run unchanged on desktop MicroPython and on a board too --
this target is the only one of the three that CI can reach. This file is the
CI half; the probe is the artifact the pin-move build runs against the native
side, which is where the C change in `src/audiomixer/` gets proven.
"""

import os
import subprocess
import sys
import unittest


PROBE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "parity", "mixer_short_loop_probe.py")
TIMEOUT = 20


class MixerShortLoop(unittest.TestCase):
    def test_the_probe_passes_on_this_target(self):
        try:
            result = subprocess.run(
                [sys.executable, PROBE], timeout=TIMEOUT,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        except subprocess.TimeoutExpired:
            self.fail(
                "mixer_short_loop_probe.py did not finish in %d s. That is the"
                " audioif#85 shape itself: a looping sample shorter than one"
                " 32-bit word spinning the mix-down. Nothing was printed"
                " because a killed process does not flush." % TIMEOUT)
        output = result.stdout.decode()
        self.assertEqual(result.returncode, 0, output)
        # Belt and braces: a probe that stopped reporting would exit 0 too.
        self.assertIn("0 failure(s)", output)
        self.assertEqual(output.count(" ok "), 6, output)


if __name__ == "__main__":
    unittest.main()
