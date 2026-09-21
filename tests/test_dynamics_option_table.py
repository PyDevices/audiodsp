"""The three option tables for `audiodynamics` must agree, name for number.

`Dynamics` takes its settings by keyword, and each keyword has to reach the
same slot in `audiodsp_dynamics_configure()` on all three targets. Three tables
say which slot:

    src/shared/audiodsp_dynamics.h   the C enum -- the authority
    src/audiodynamics/Dynamics.c    MicroPython, name -> enum constant
    src/cpython/audiodynamics.py    the CPython twin, name -> a LITERAL INTEGER
    src/circuitpython_spike/shared-bindings/audiodynamics/Dynamics.c
                                    the CircuitPython binding, name -> constant

**There are four, and this file first said three.** The fourth was found by the
three-way `verify_dsp` run on 2026-09-09: `gain_smooth_ms` and
`feedback_gain_corrected` had been added to the enum, to MicroPython and to the
CPython twin, and the patched CircuitPython build refused both with
`TypeError: unknown Dynamics option`. Exactly what a third target is for, and
exactly the drift this file exists to prevent - it just did not know where to
look yet.

Only the middle one is safe. It names the enum constants, so a typo is a
compile error and a reordered enum moves with it. The CPython twin writes the
numbers out by hand -- `"feedback_detector": 17` -- and its own comment says
why that is load-bearing: *"these numbers are the C enum's, and the C enum only
ever grows."* Nothing enforced it. An option appended to the enum in the wrong
place, or a number typed one out, would send `sidechain_lp_hz` to
`feedback_detector`'s slot on the CPython target only, and the parity gates
would report it as a DSP divergence rather than as a table error.

This is the same shape as `test_voice_ceiling_consistency.py`: several
independent copies of one fact, no mechanism making them agree, so a check
over them is the mechanism.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent

HEADER = ROOT / "src" / "shared" / "audiodsp_dynamics.h"
BINDING = ROOT / "src" / "audiodynamics" / "Dynamics.c"
SPIKE = (ROOT / "src" / "circuitpython_spike" / "shared-bindings"
         / "audiodynamics" / "Dynamics.c")
TWIN = ROOT / "src" / "cpython" / "audiodynamics.py"

PREFIX = "AUDIODSP_DYNAMICS_OPT_"


def enum_order():
    """The option names in `audiodsp_dynamics_option_t`, in declaration order,
    with the `_COUNT` sentinel dropped."""
    text = HEADER.read_text()
    body = re.search(r"typedef enum\b(.*?)\}\s*audiodsp_dynamics_option_t;",
                     text, re.DOTALL)
    if body is None:
        raise AssertionError("no audiodsp_dynamics_option_t in %s" % HEADER)
    names = re.findall(PREFIX + r"[A-Z0-9_]+", body.group(1))
    if names[-1] != PREFIX + "COUNT":
        raise AssertionError(
            "the enum's last member is %s, not the _COUNT sentinel; a new "
            "option was appended after it, which makes every count wrong"
            % names[-1])
    return names[:-1]


def table_pairs(path):
    """`{python name: enum name}` out of a C binding's option table."""
    found = re.findall(r"\{\s*MP_QSTR_([A-Za-z0-9_]+)\s*,\s*("
                       + PREFIX + r"[A-Z0-9_]+)\s*\}", path.read_text())
    return dict(found)


def binding_pairs():
    """The MicroPython table."""
    return table_pairs(BINDING)


def spike_pairs():
    """The CircuitPython table."""
    return table_pairs(SPIKE)


def twin_numbers():
    """`{python name: slot}` out of the CPython twin's `_OPTIONS`."""
    text = TWIN.read_text()
    body = re.search(r"^_OPTIONS = \{(.*?)^\}", text, re.DOTALL | re.MULTILINE)
    if body is None:
        raise AssertionError("no _OPTIONS mapping in %s" % TWIN)
    found = re.findall(r'"([A-Za-z0-9_]+)"\s*:\s*(\d+)', body.group(1))
    return dict((name, int(slot)) for name, slot in found)


class TheThreeOptionTablesAgree(unittest.TestCase):

    def setUp(self):
        self.enum = enum_order()
        self.slots = dict((name, index)
                          for index, name in enumerate(self.enum))

    def test_the_enum_is_read_at_all(self):
        """A control, so a regex that silently matched nothing cannot make
        every other test here vacuous."""
        self.assertGreater(len(self.enum), 25)
        self.assertEqual(self.enum[0], PREFIX + "THRESHOLD_DB")

    def test_every_micropython_name_is_a_real_enum_member(self):
        pairs = binding_pairs()
        self.assertGreater(len(pairs), 25)
        for name, enum_name in sorted(pairs.items()):
            with self.subTest(option=name):
                self.assertIn(enum_name, self.slots)

    def test_the_two_bindings_expose_the_same_option_names(self):
        micropython = set(binding_pairs())
        cpython = set(twin_numbers())
        self.assertEqual(
            micropython, cpython,
            "an option reachable on one target and not the other: "
            "MicroPython only %s, CPython only %s"
            % (sorted(micropython - cpython), sorted(cpython - micropython)))

    def test_the_twins_numbers_are_the_enums_positions(self):
        """The one that can drift silently, and the reason this file exists."""
        pairs = binding_pairs()
        for name, slot in sorted(twin_numbers().items()):
            with self.subTest(option=name):
                enum_name = pairs.get(name)
                self.assertIsNotNone(
                    enum_name, "%s is in the CPython table and not in the "
                    "MicroPython one" % name)
                self.assertEqual(
                    slot, self.slots[enum_name],
                    "the CPython twin sends %r to slot %d; the C enum has %s "
                    "at %d. Whatever lives at %d would be set instead."
                    % (name, slot, enum_name, self.slots[enum_name], slot))

    def test_every_enum_option_is_reachable_from_both_targets(self):
        """An option in the kernel that neither binding exposes cannot be set
        from Python at all: either somebody stopped half-way through adding
        it, or it is dead. Found by planting exactly that fault against an
        earlier draft of this file, which passed it - comparing the two
        bindings to *each other* says nothing when both are missing the same
        name."""
        exposed = set(binding_pairs().values()) | set(spike_pairs().values())
        missing = [name for name in self.enum if name not in exposed]
        self.assertEqual(
            missing, [],
            "in the C enum and reachable from no target: %s" % (missing,))

    def test_the_circuitpython_binding_exposes_the_same_names(self):
        """The fourth table, and the one that was silently behind. A patched
        CircuitPython build refusing an option the other two accept is not a
        build problem: it is this table."""
        micropython = set(binding_pairs())
        circuitpython = set(spike_pairs())
        self.assertGreater(len(circuitpython), 25)
        self.assertEqual(
            micropython, circuitpython,
            "an option reachable on MicroPython and not on CircuitPython, or "
            "the reverse: MicroPython only %s, CircuitPython only %s"
            % (sorted(micropython - circuitpython),
               sorted(circuitpython - micropython)))

    def test_the_circuitpython_names_match_their_enum_members(self):
        for name, enum_name in sorted(spike_pairs().items()):
            with self.subTest(option=name):
                self.assertEqual(enum_name, PREFIX + name.upper())

    def test_the_names_match_their_enum_members(self):
        """`sidechain_hz` must be `..._OPT_SIDECHAIN_HZ`, not some other
        member that happens to sit at the right index. Two options swapped in
        both tables at once would satisfy every check above."""
        for name, enum_name in sorted(binding_pairs().items()):
            with self.subTest(option=name):
                self.assertEqual(enum_name, PREFIX + name.upper())


if __name__ == "__main__":
    unittest.main()
