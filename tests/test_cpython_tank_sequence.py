"""audioverb's native TankState reads its delay and tap tables safely.

`delays` and `taps` are read one number at a time, and reading a number can
run Python: an item's `__float__`. If that code changes the list (or another
thread does), the table being read must not change under the reader. It used
to read the list's items in place, so a list emptied mid-read crashed the
interpreter. Run in a child interpreter, so a crash fails this test instead of
ending the run.
"""

import subprocess
import sys
import textwrap
import unittest

CHILD = textwrap.dedent("""
    import _audiodsp

    def shrinking(numbers):
        # A list whose first item empties the list as it is read.
        values = []

        class Shrinking:
            def __float__(self):
                values.clear()
                return numbers[0]

        values.append(Shrinking())
        values.extend(float(number) + 0.0 for number in numbers[1:])
        return values

    delays = [400.0 + 7 * index for index in range(12)]
    taps = [0.0, 0.0, 10.0, 0.5, 1.0, 1.0, 20.0, 0.5]
    _audiodsp.TankState(sample_rate=48000, delays=shrinking(delays),
                        taps=shrinking(taps))
    state = _audiodsp.TankState(sample_rate=48000)
    state.recut(shrinking(delays), None)
    state.recut(None, shrinking(taps))
    print("read")
""")


class TankSequenceTest(unittest.TestCase):
    def test_a_table_emptied_while_it_is_read(self):
        result = subprocess.run([sys.executable, "-c", CHILD],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(
            (result.returncode, result.stdout.strip()), (0, "read"),
            "child interpreter: %s" % result.stderr[-2000:])


if __name__ == "__main__":
    unittest.main()
