"""Our two hand-written bindings per module must expose the same surface.

audioif carries each of its nine own modules twice: `src/<module>/*.c` binds it
for MicroPython, and `src/circuitpython_spike/shared-bindings/<module>/*.c`
binds it for the patched CircuitPython build. They are separate files with
separate locals dicts, and until this file existed nothing held them to each
other.

**Three changes drifted in one day.** On 2026-09-09 `gain_smooth_ms`
(audioif#61), `feedback_gain_corrected` (audioif#62) and `Convolver.latency`
(audioif#44) all went into the MicroPython binding and not the CircuitPython
one, and `deinit()` on thirteen node types (audioif#58/#60/#63) did the same -
so three closed issues were closed on two of three targets. Every one was found
by the *first* three-way `verify_dsp` run rather than by a check, which is
audioif#75. Three in a day is a pattern, and the fourth is cheaper to prevent
than to find.

## What this compares, and what it deliberately does not

It reads the `MP_QSTR_` keys out of every locals dict in both bindings and
requires the sets to match, type for type. That catches a method or property
added to one side and not the other, which is what all four instances were.

It does not compare *behaviour* - two bindings can expose the same names and do
different things. That is `verify_dsp`'s job: every target renders the probes
identically. Nor does it compare argument lists, which are not in the locals
dict. Both are worth having and neither is here; this is the cheap half that
would have caught every instance so far.

`AUDIOSAMPLE_FIELDS` is a macro rather than a key, so its presence is compared
as a name of its own: a binding that dropped it would lose `sample_rate`,
`bits_per_sample` and `channel_count` in one line without any key changing.
"""

import glob
import os
import re

import _audioif
import importlib
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent

#: The ten modules that are ours. Upstream CircuitPython has no counterpart to
#: any of them, which is why we carry two bindings rather than one.
MODULES = ("audiobiquad", "audioconvolve", "audiodynamics", "audioecho",
           "audioladder", "audiomath", "audiomodal", "audioroute",
           "audioshaper", "audioverb")

#: Both spellings are in use: audioif's MicroPython bindings mostly say
#: `_locals_table` and the CircuitPython ones `_locals_dict_table`. A regex that
#: knew only one of them matched nothing on one side and reported every type as
#: "only in CircuitPython" - which looked like a catastrophic finding and was a
#: broken pattern.
TABLE = re.compile(
    r"static const mp_rom_map_elem_t\s+(\w+?)_locals(?:_dict)?_table\[\]\s*"
    r"=\s*\{(.*?)\n\};", re.DOTALL)
KEY = re.compile(r"MP_ROM_QSTR\(MP_QSTR_(\w+)\)")


def surfaces(paths, skip):
    """`{type prefix: set of exposed names}` across a binding's source files."""
    found = {}
    for path in sorted(paths):
        if os.path.basename(path) == skip:
            continue
        text = pathlib.Path(path).read_text()
        for match in TABLE.finditer(text):
            keys = set(KEY.findall(match.group(2)))
            if "AUDIOSAMPLE_FIELDS" in match.group(2):
                keys.add("<AUDIOSAMPLE_FIELDS>")
            found[match.group(1)] = keys
    return found


def micropython_surfaces(module):
    return surfaces(glob.glob(str(ROOT / "src" / module / "*.c")), "module.c")


def circuitpython_surfaces(module):
    return surfaces(
        glob.glob(str(ROOT / "src" / "circuitpython_spike" / "shared-bindings"
                      / module / "*.c")), "__init__.c")


class TheTwoBindingsExposeTheSameSurface(unittest.TestCase):

    def test_the_tables_are_found_at_all(self):
        """A control, because the failure mode of this file is a regex that
        matches nothing and then agrees with itself. Both sides must yield the
        sixteen types.

        The count is deliberately a literal rather than a comparison of the two
        sides against each other: a node added to one binding and not the other
        would make both sides agree on thirteen and this test is what says how
        many there are meant to be. It went from fourteen to fifteen when
        `audioshaper.SampleHold` landed (audioif#97), and to sixteen with
        `audioroute.Port` -- which is the first node the live-audio-path
        spike added, and the first one whose two bindings differ on
        purpose: the MicroPython copy takes the pump lock around `play()`
        and `deinit()` and the CircuitPython copy has no lock to take.
        The surface is the same, which is all this file compares."""
        micropython = {}
        circuitpython = {}
        for module in MODULES:
            micropython.update(micropython_surfaces(module))
            circuitpython.update(circuitpython_surfaces(module))
        self.assertEqual(len(micropython), 16, sorted(micropython))
        self.assertEqual(len(circuitpython), 16, sorted(circuitpython))

    def test_every_type_exists_on_both_sides(self):
        for module in MODULES:
            with self.subTest(module=module):
                micropython = set(micropython_surfaces(module))
                circuitpython = set(circuitpython_surfaces(module))
                self.assertEqual(
                    micropython, circuitpython,
                    "a type bound on one target and not the other: "
                    "MicroPython only %s, CircuitPython only %s"
                    % (sorted(micropython - circuitpython),
                       sorted(circuitpython - micropython)))

    def test_every_type_exposes_the_same_names(self):
        """The one that would have caught all four of the day's instances."""
        for module in MODULES:
            micropython = micropython_surfaces(module)
            circuitpython = circuitpython_surfaces(module)
            for name in sorted(set(micropython) & set(circuitpython)):
                with self.subTest(type=name):
                    self.assertEqual(
                        micropython[name], circuitpython[name],
                        "%s exposes different names on the two targets: "
                        "MicroPython only %s, CircuitPython only %s"
                        % (name,
                           sorted(micropython[name] - circuitpython[name]),
                           sorted(circuitpython[name] - micropython[name])))

    def test_every_node_type_has_deinit_on_both_sides(self):
        """Named rather than implied, because it is the instance that let three
        closed issues be closed on two targets of three."""
        for module in MODULES:
            for label, surface in (("MicroPython", micropython_surfaces(module)),
                                   ("CircuitPython",
                                    circuitpython_surfaces(module))):
                for name, keys in sorted(surface.items()):
                    with self.subTest(target=label, type=name):
                        self.assertIn("deinit", keys)
                        self.assertIn("__enter__", keys)
                        self.assertIn("__exit__", keys)


class EveryModuleSaysWhichAudioifItIs(unittest.TestCase):
    """audioif#55: a firmware has to be able to name the audioif it was built
    from, and `os.uname().version` answers for MicroPython only.

    The point of testing it here rather than trusting nine edits: a module added
    later, or one whose globals table is rewritten, fails this instead of
    shipping unmarked. That was the actual failure mode -- on 2026-09-09 a
    firmware built from a throwaway source tree during a pin move could not be
    attributed to any commit, because nothing in it recorded one.

    Upstream's modules are deliberately NOT checked: adding a dunder to
    `audiocore` or `synthio` would deviate from CircuitPython's surface, and any
    of our nine answers the question (Brad, 2026-09-09).

    **The CircuitPython spike bindings are also excluded, on purpose.** They
    have the same globals-table shape and could carry the marker, but the CP
    build has no `-DAUDIOIF_*` plumbing and does not need any: that binary is a
    local comparison artifact, not shipped firmware, and its exact bytes are
    already pinned with written provenance in
    `tests/test_voice_ceiling_consistency.py`. This is a scope decision and not
    an instance of the drift audioif#75 is about -- that issue is about
    functional surface (a method or an option present on one binding and not the
    other), and build metadata is not that.
    """

    #: The one line each module.c carries; see src/cp_compat/audioif_build.h.
    MARKER = "AUDIOIF_BUILD_GLOBALS"

    def test_every_one_of_our_modules_carries_the_marker(self):
        for module in MODULES:
            with self.subTest(module=module):
                source = (ROOT / "src" / module / "module.c").read_text()
                self.assertIn(
                    self.MARKER, source,
                    "%s/module.c does not expose __version__/__revision__, so a "
                    "firmware built with it cannot be attributed" % module)

    def test_the_python_twins_answer_the_same_way(self):
        for module in MODULES:
            with self.subTest(module=module):
                twin = importlib.import_module(module)
                self.assertEqual(twin.__version__, _audioif.__version__)
                self.assertEqual(twin.__revision__, _audioif.__revision__)

    def test_upstream_modules_are_left_alone(self):
        """The control: this suite must not pass by marking everything."""
        for module in ("audiocore", "audiodelays", "audiofilters", "audiomixer"):
            with self.subTest(module=module):
                source = (ROOT / "src" / module / "module.c").read_text()
                self.assertNotIn(self.MARKER, source)

    def test_the_version_is_not_a_placeholder(self):
        """`__version__` comes from the VERSION file and is always available."""
        self.assertNotEqual(_audioif.__version__, "0.0.0+unknown")

    def test_the_revision_is_real_when_the_build_could_know_it(self):
        """`unknown` is a real answer for a wheel, and a bug in a checkout build.

        `__revision__` identifies a BUILD; `__version__` identifies a RELEASE.
        A published wheel is built by `python -m build`, which builds from an
        unpacked sdist with no `.git`, so `git describe` cannot run and the
        honest answer is `unknown` -- the version already names that artifact
        exactly. Firmware and in-place builds do run from a checkout, and that is
        the case audioif#55 needed: attributing a board digest to a commit.

        So this asserts the value only when the extension under test was built
        from this checkout, and skips otherwise. It cost a red release run
        (v0.4.0, every OS and Python) by asserting the value unconditionally --
        CI installs a wheel, so `unknown` was correct there and the test was
        wrong.
        """
        try:
            described = subprocess.run(
                ["git", "-C", str(ROOT), "describe", "--always", "--dirty",
                 "--abbrev=7"],
                capture_output=True, text=True, check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("not a git checkout, so no revision is knowable")
        if not described:
            self.skipTest("git described nothing")
        # Built in place, or installed from a wheel elsewhere?
        try:
            in_tree = pathlib.Path(_audioif.__file__).resolve().is_relative_to(
                ROOT.resolve())
        except AttributeError:                      # Python < 3.9
            in_tree = str(ROOT.resolve()) in str(
                pathlib.Path(_audioif.__file__).resolve())
        if not in_tree:
            self.skipTest("_audioif was installed, not built from this tree; "
                          "`unknown` is correct for a wheel")
        # NOT an equality check against `described`. The revision is compiled in,
        # so it names the commit the extension was BUILT at, and goes stale the
        # moment anything is committed after it -- an equality bar would fail on
        # every commit until someone rebuilt, which trains people to ignore it.
        # What is worth asserting is that the mechanism produced something.
        self.assertNotEqual(_audioif.__revision__, "unknown")
        self.assertTrue(_audioif.__revision__.strip())


if __name__ == "__main__":
    unittest.main()
