"""`audioif_util`: one number, the same number, on every target.

A setting written in Python is held at the interpreter's `mp_float_t` width --
double here and on a desktop MicroPython, single on every board audioif ships
on and on a MicroPython built with `MICROPY_FLOAT_IMPL_FLOAT`. `float32` is the
round trip that makes the two agree, and `float32_bits` is the exact way to
print one. `docs/correctness-standard.md` carries the rule; audioif#80 is where
six parity probes were found disagreeing with CPython over nothing else.

| ID | Trait | Bar |
|---|---|---|
| U1 | `float32` is the identity on anything a single-precision target holds | exact, over every value this file rounds |
| U2 | `float32` rounds on a double host | 0.35, 0.7 and 0.6 all move, and move to the single nearest to them |
| U3 | The result is a true single, not a shortened decimal | bit-equal to `struct`'s own round trip for 10000 values |
| U4 | `float32_bits` is eight hex characters of the same single | exact, and it agrees with `float32` on every value |
| U5 | The parity probes' own constants are covered | every literal the six probes pass through it |

**What CPython cannot prove here, and where it is proved instead.** That the
call is the *identity* on a genuinely single-precision interpreter is U1 stated
against values this host has rounded; the interpreter-level version of the same
claim is `tests/parity/verify_dsp.py` on a
`-DMICROPY_FLOAT_IMPL=MICROPY_FLOAT_IMPL_FLOAT` build, where four probes that
disagreed with CPython now agree because of these calls. One run of that gate
is worth more than anything this file can assert, and this file is what CI can
run.

**The checker can fail.** U2 is what catches `float32 = lambda x: x`, U3 is what
catches a decimal `round(x, 7)` standing in for it, and U4 catches a bit
pattern taken at the wrong width -- all three were tried against a deliberately
wrong implementation before this file was trusted.
"""

import struct
import unittest

from audioif_util import float32, float32_bits


#: Every inexact literal the six probes of audioif#80 hand to a node, plus the
#: awkward ends. A value that is exact in both widths (0.0, 0.5, 1.0, 4.0) is
#: here to say the call leaves it alone.
PROBE_CONSTANTS = (0.35, 0.6, 0.7, 0.85, 0.4, 0.7079, 1.4125,
                   0.0, 0.5, 1.0, 4.0, -0.35, 1e-30, 3.4e38)


class Float32(unittest.TestCase):
    def test_u1_identity_on_single_precision_values(self):
        """U1: idempotent, so a target that already holds singles is untouched."""
        for value in PROBE_CONSTANTS:
            once = float32(value)
            self.assertEqual(float32(once), once, value)
            self.assertEqual(float32(float32(once)), once, value)

    def test_u2_rounds_on_a_double_host(self):
        """U2: and it rounds to the nearest single, not merely to something."""
        for value in (0.35, 0.7, 0.6, 0.85, 0.4, 0.7079, 1.4125):
            self.assertNotEqual(float32(value), value, value)
            neighbours = []
            bits = struct.unpack("<I", struct.pack("<f", value))[0]
            for delta in (-1, 0, 1):
                neighbours.append(struct.unpack(
                    "<f", struct.pack("<I", bits + delta))[0])
            self.assertEqual(
                min(neighbours, key=lambda candidate: abs(candidate - value)),
                float32(value), value)

    def test_u2_exact_values_are_untouched(self):
        for value in (0.0, 0.5, 1.0, 4.0, -2.0, 1200.0, 8000.0):
            self.assertEqual(float32(value), value, value)

    def test_u3_is_a_true_single(self):
        """U3: bit-equal to struct's round trip, over a wide sweep."""
        for index in range(10000):
            value = (index - 5000) / 997.0
            self.assertEqual(
                struct.pack("<f", float32(value)), struct.pack("<f", value),
                value)

    def test_u4_bits_are_eight_hex_characters_of_the_same_single(self):
        for value in PROBE_CONSTANTS:
            bits = float32_bits(value)
            self.assertEqual(len(bits), 8, value)
            self.assertEqual(bits, bits.lower(), value)
            self.assertEqual(struct.unpack("<f", bytes.fromhex(bits))[0],
                             float32(value), value)

    def test_u4_bits_are_the_value_a_probe_prints(self):
        """The one recorded in dynamics_probe's comment, so it stays true."""
        self.assertEqual(float32_bits(-5.836907386779785), "f2c7bac0")
        self.assertEqual(float32_bits(0.35), "3333b33e")

    def test_u5_probe_constants_round_trip_through_a_pack(self):
        """U5: nothing the probes pass is outside the range this can hold."""
        for value in PROBE_CONSTANTS:
            self.assertEqual(float32(value),
                             struct.unpack("<f", struct.pack("<f", value))[0],
                             value)


if __name__ == "__main__":
    unittest.main()
