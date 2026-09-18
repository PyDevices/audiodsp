"""The voice ceiling lives in five places. This asserts they agree.

Why this test exists, and why it is not paranoia:

The four older parity gates in this repository are **byte-identical at N=14,
36 and 56** -- measured, not assumed. `verify_acceptance`, `verify_effects`,
`verify_streaming`, `verify_biquad` and the rest of this suite render material
that never fills 14 channels and never crosses the mix-down knee at +/-28000,
so none of them can observe a change to CIRCUITPY_SYNTHIO_MAX_CHANNELS at all.

Since 2026-09-06 a fifth gate does cross the knee --
`tests/parity/verify_mixdown_knee.py`, audioif#27 -- but it sees exactly ONE
of the five sites. The CPython target reads only `src/cpython/synthio.py`'s
`max_polyphony`, which it hands to `_audioif.mixdown_i32` as the limiter
divisor; the header, `micropython.mk` and `micropython.cmake` copies are read
by no CI gate whatever.

So a ceiling change applied to three of the five sites still ships GREEN, and
nothing else in CI would say a word. This test remains the only thing standing
between a HALF-APPLIED ceiling and a release. What the knee gate adds is the
other failure -- a ceiling moved consistently across all five sites, which
this test passes by construction and which silently changes the audio.

The five sites are genuinely independent -- three build paths plus two
constants inside the CPython target, which does not read the header
(setup.py builds _audioif from src/cpython/ and src/shared/ only, never from
src/synthio/). There is no mechanism making them agree; only this check.

DELIBERATELY NOT INCLUDED: `_audioif.c`'s `0x0fffffff / (32768 * 2 - 28000)`
is SYNTHIO_MIX_DOWN_SCALE(2) faithfully mirroring upstream CircuitPython's
own two-channel default in a code path that is not ours to re-tune. It is a
literal 2 that must stay 2. If you are here because you changed the ceiling
and this test still passed, check that you did not "fix" that line too.
"""

import hashlib
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]

#: The pinned CircuitPython oracle binary, and its hash.
#:
#: The oracle is built at the SAME ceiling this port ships (64) and at the same
#: CircuitPython version the port tracks (10.3.0). It is rebuilt deliberately
#: whenever either moves, and re-pinned in that change -- the old rule
#: ("built at 14 and must never be rebuilt") is retired: an oracle at a
#: different configuration cannot answer the only question worth asking, which
#: is whether we render CircuitPython's bytes in the same situation. See
#: docs/correctness-standard.md. The hole this closes is unchanged: a
#: CFLAGS_EXTRA rebuild of the oracle needs no edit to any tracked file,
#: leaves `git -C cmods/circuitpython status` clean, and overwrites the
#: gitignored binary in place. Every existing check would pass on a silently
#: different oracle. Comparing the bytes is the only thing that notices.
#:
#: Re-pinned and MOVED 2026-09-17 for audioif#89. Two things changed. The
#: **path**: `cmods/bin/circuitpython` is what `cmods/build_interpreters.sh`'s
#: `cp-unix` target installs, so anyone refreshing the workspace interpreters
#: silently replaced the oracle with a coverage build at the variant's own
#: 14-voice ceiling. That happened twice in eight days, and it is the whole of
#: #89. The oracle now lives where no build script writes it by accident: the
#: opt-in `cp-oracle` target, added to `build_interpreters.sh` in the same
#: change, builds at 64 and installs only to `bin/circuitpython-oracle-<ver>`.
#: `bin/circuitpython` belongs to the installer and may change under you.
#: The **bytes**: PR #86 gave `src/shared/` an fp-contract header, and
#: `src/shared/` is relinked into this binary by `apply_cp_patches.sh`, so our
#: own kernels moved under CircuitPython's untouched sources again. (PR #93's
#: `audiospeed` fix is ours alone -- the CP tree keeps upstream's SpeedChanger,
#: which is why `speedchanger_hold_probe.py` skips CircuitPython.) Built from
#: CircuitPython 10.3.0 at `CIRCUITPY_SYNTHIO_MAX_CHANNELS=64` with audioif at
#: 977ef26. Verified: it answers 64 voices; CircuitPython's own modules are
#: byte-identical across the rebuild (`synthtools_acceptance` and
#: `mixdown_knee`'s stored `circuitpython_stdout` both reproduce exactly); and
#: `verify_dsp` agrees three ways across 45 comparisons, with no skip beyond
#: the three already written down. Previous: 447e3ee88a143e1d, still on disk
#: as `cmods/bin/circuitpython-effects-10.3.0`.
#:
#: A coverage build is not byte-reproducible, which is the other reason this
#: pins a file and not a recipe. Two builds of the same tree with the same
#: flags differ in 1476 of 27,831,104 bytes: the 20-byte GNU build-id, and one
#: 4-byte gcov stamp per translation unit in `.data`. `.text` and every other
#: section are identical. So rebuilding and comparing hashes cannot verify this
#: pin -- reproducing the stored captures on the binary is what does.
#:
#: Re-pinned again 2026-09-09, later the same day, for audioif#64: the
#: `audiobiquad` float biquad moved to transposed direct form II, and
#: `audiobiquad` is one of the nine modules `apply_cp_patches.sh` adds to the CP
#: tree -- so a change to our own shared kernel relinks this binary even though
#: CircuitPython's own sources are untouched. Verified: CircuitPython's own
#: modules are byte-identical across the rebuild (`synthtools_acceptance` and
#: `mixdown_knee`'s stored `circuitpython_stdout` both reproduce exactly), and
#: `verify_dsp` agrees on all three interpreters again. Previous:
#: 94500bc0382f1cc2.
#:
#: Re-pinned 2026-09-09 for the deliberate rebuild to CircuitPython 10.3.0 at
#: CIRCUITPY_SYNTHIO_MAX_CHANNELS=64, replacing the 10.2.1-at-14 build
#: b3063621c72a085b (kept at cmods/bin/circuitpython-oracle-10.2.1). That
#: rebuild is what found the two synthio/audiomixer behaviour changes 10.3.0
#: made and this port had not taken: the panning polarity flip and the
#: zero-crossing loudness gate.
#:
#: It noticed once (#33). The hash was first recorded at dda8a77 on
#: 2026-09-03 20:46; the binary was relinked at 23:05 the same evening by the
#: jpegio Phase 2 landing (an aggregator reinstall adding lvgl, pygraphics and
#: usdl2 -- nothing audio), and the pin went red for every session after.
#: The relinked binary still answers 14 voices and still reproduces both
#: committed reference digests, so it is re-pinned to the bytes it has, with
#: this note as the provenance the relink never recorded. A future mismatch
#: means what it meant then: find what rebuilt it, ask it the ceiling, and
#: re-pin with the reason written down.
#:
#: Re-pinned 2026-09-18 01:45: rebuilt with `build_interpreters.sh --only
#: cp-oracle` from main at 7b4beab (DSP identical to e3b95e7) because the
#: previous oracle predated audioshaper.SampleHold (PR #98) and verify_dsp's
#: three-way run aborted on its samplehold probe. Still 64 voices; the
#: build script refused to install anything that answered otherwise.
ORACLE = ROOT.parent / "cmods" / "bin" / "circuitpython-oracle-10.3.0"
ORACLE_SHA256 = (
    "9ac258304347341875208813b510145a0dbf45ffd910b39457749fa165dd7920")


def _search(relative, pattern):
    """The single integer `pattern` captures in `relative`, or a failure."""
    path = ROOT / relative
    text = path.read_text()
    found = re.findall(pattern, text, re.M)
    if len(found) != 1:
        raise AssertionError(
            "%s: expected exactly one match for %r, found %d. The ceiling "
            "moved or the file was restructured; fix this test deliberately "
            "rather than loosening the pattern." % (relative, pattern,
                                                    len(found)))
    return int(found[0]), path


#: (label, path, regex capturing the ceiling as group 1).
SITES = (
    ("header default",
     "src/synthio/__init__.h",
     r"^#define CIRCUITPY_SYNTHIO_MAX_CHANNELS \((\d+)\)"),
    ("Make ports",
     "micropython.mk",
     r"^CFLAGS_USERMOD \+= -DCIRCUITPY_SYNTHIO_MAX_CHANNELS=(\d+)"),
    ("CMake ports",
     "micropython.cmake",
     r"INTERFACE CIRCUITPY_SYNTHIO_MAX_CHANNELS=(\d+)\)"),
    ("CPython target, admission",
     "src/cpython/synthio.py",
     r"^    max_polyphony = (\d+)"),
    ("CPython target, extension default",
     "src/cpython/_audioif.c",
     r"^    unsigned int max_polyphony = (\d+);"),
)


class VoiceCeilingConsistency(unittest.TestCase):

    def test_all_five_sites_agree(self):
        values = {}
        for label, relative, pattern in SITES:
            value, path = _search(relative, pattern)
            values[label] = (value, "%s" % relative)

        distinct = {v for v, _ in values.values()}
        if len(distinct) != 1:
            lines = ["the voice ceiling disagrees across its five sites:"]
            for label, (value, where) in values.items():
                lines.append("    %-34s %3d   (%s)" % (label, value, where))
            lines.append("")
            lines.append(
                "    A patch behaves differently depending on how audioif was "
                "built. No other gate here can see this. "
                "tests/parity/verify_mixdown_knee.py crosses the mix-down "
                "knee and so notices a ceiling change, but only in "
                "src/cpython/synthio.py -- the one site the CPython target "
                "reads. A disagreement among the other four is invisible to "
                "it, so if you are reading this message, this test is still "
                "the only reason you know.")
            raise AssertionError("\n".join(lines))

    def test_cpython_admission_and_extension_default_agree(self):
        """These two are one number wearing two hats, and they can drift.

        `synthio.py`'s value gates admission AND is passed to
        `_audioif.mixdown_i32` as the limiter's divisor. `_audioif.c`'s is the
        default for callers who pass nothing. If they diverge, a caller using
        the default limits to a different voice count than the engine admits.
        """
        admission, _ = _search(SITES[3][1], SITES[3][2])
        default, _ = _search(SITES[4][1], SITES[4][2])
        self.assertEqual(
            admission, default,
            "src/cpython/synthio.py:max_polyphony (%d) and "
            "src/cpython/_audioif.c's default (%d) must match: the first "
            "feeds mixdown_i32 as the limiter divisor, the second is what a "
            "caller gets who passes nothing."
            % (admission, default))

    def test_oracle_binary_is_the_pinned_one(self):
        if not ORACLE.exists():
            self.skipTest("oracle binary absent at %s" % ORACLE)
        digest = hashlib.sha256(ORACLE.read_bytes()).hexdigest()
        self.assertEqual(
            digest, ORACLE_SHA256,
            "\n    the CircuitPython oracle binary is not the pinned build."
            "\n      expected %s"
            "\n      got      %s"
            "\n"
            "\n    Every parity golden in this repository is measured against "
            "this binary. A rebuild with different flags -- a raised "
            "CIRCUITPY_SYNTHIO_MAX_CHANNELS, say -- changes what 'parity' "
            "means while leaving every tracked file untouched and every git "
            "tree clean. That is why this compares bytes and not a commit."
            "\n"
            "\n    Nothing but `build_interpreters.sh --only cp-oracle` "
            "should ever write this path, and that target exists so no "
            "routine interpreter refresh can. `cmods/bin/circuitpython` is "
            "the installer's coverage build at 14 voices and is NOT this."
            "\n"
            "\n    If you rebuilt it deliberately, that is a decision for "
            "Brad and it needs its own commit saying why, with this hash "
            "updated in the same change." % (ORACLE_SHA256, digest))


if __name__ == "__main__":
    unittest.main()
