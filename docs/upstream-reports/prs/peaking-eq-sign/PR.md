# PR for adafruit/circuitpython #11265

Branch: `fix-peaking-eq-sign` (one commit; the patch file beside this
document). Verified against upstream `main` at `d897c15f` on 2026-08-31.

## Title

```
Fix sign of b2 in synthio PEAKING_EQ biquad
```

## Body

```markdown
Fixes #11265.

`common_hal_synthio_biquad_tick()` computes the PEAKING_EQ numerator
with `b2 = 1 + alpha * A`; the RBJ cookbook peaking section is

    b0 = 1 + alpha*A    b1 = -2*cos(w0)    b2 = 1 - alpha*A
    a0 = 1 + alpha/A    a1 = -2*cos(w0)    a2 = 1 - alpha/A

The minus is what makes numerator and denominator sum to the same value
at DC and again at Nyquist — unity everywhere except the band around
`f0`. With the plus, the numerator picks up `2*alpha*A` at DC that the
denominator does not, so the "bell" is actually a bass shelf with a
bell buried in it.

Measured on a unix coverage build of `main`, a +6 dB bell at 1 kHz,
Q 1, 48 kHz (`audiofilters.Filter` fed DC and sine `RawSample`s):

|            | before    | after   | should be |
|------------|-----------|---------|-----------|
| DC         | +21.39 dB | −0.11   | 0.00      |
| centre     | +23.42 dB | +5.94   | +6.00     |
| 12 kHz     | −0.01 dB  | +0.03   | 0.00      |

The other six filter modes already match the cookbook and are
untouched. The wrong coefficient reaches both `audiofilters.Filter`
and `synthio.Note.filter`, since they share these coefficients.

Includes a regression test (`tests/circuitpython/synthio_biquad_peaking.py`)
that checks the DC, centre, and 12 kHz gains; it fails on current
`main` and passes with the fix. The neighboring biquad/filter tests
that pass on unmodified `main` in the same environment still pass.
```

## Verification transcript (2026-08-31, upstream main d897c15f)

Before (unmodified `main`, unix coverage build):

```
$ ports/unix/build-coverage/micropython repro_peaking.py
a +6 dB bell at 1000 Hz, Q 1, 48000 Hz
  DC       +21.39 dB   (should be 0.00)
  centre   +23.42 dB   (should be +6.00)
  12 kHz    -0.01 dB   (should be 0.00)
```

After (same build with the one-character fix):

```
a +6 dB bell at 1000 Hz, Q 1, 48000 Hz
  DC        -0.11 dB   (should be 0.00)
  centre    +5.94 dB   (should be +6.00)
  12 kHz    +0.03 dB   (should be 0.00)
```

Regression test round-trip (run-tests.py, unix coverage build):
fails on the unfixed binary, passes on the fixed one.

Note: the draft's "after" column (measured 2026-08-27) read +0.00 /
+6.00 / −0.01; today's build reads −0.11 / +5.94 / +0.03. Same fix,
same order of magnitude of residual (Q15 coefficient rounding); the PR
body above carries today's numbers only.

Pre-commit: all hooks pass on the changed files (uncrustify 0.78.1,
ruff, codespell, end-of-file, trailing-whitespace, translations —
the translations hook's rewrite of `locale/circuitpython.pot` was
partial-run churn and is not part of the commit).

Known-failing context: `synthio_biquad.py`, `audiofilter_filter_biquads.py`,
`audiofilter_filter_stereo.py`, `audiofilter_filter_stereo_biquads.py`
fail identically on unmodified `main` in this environment (float
precision vs. the checked-in .exp files) — pre-existing, not caused by
this change. `audiofilter_filter.py` and the new test pass.

## Publish commands (Brad runs these; nothing has been pushed)

From a checkout with this branch (the scratch clone lives at
`/tmp/claude-1000/-home-brad-gh-pydevices/2825dfbc-8269-4769-8e80-f7e2e1a461a1/scratchpad/upstream/circuitpython`,
but a scratch dir may be gone — `git am` the patch onto fresh `main`
instead):

```sh
git clone https://github.com/adafruit/circuitpython.git
cd circuitpython
git checkout -b fix-peaking-eq-sign
git am /path/to/audiodsp/docs/upstream-reports/prs/peaking-eq-sign/0001-Fix-sign-of-b2-in-synthio-PEAKING_EQ-biquad.patch

gh repo fork adafruit/circuitpython --remote --remote-name fork
git push fork fix-peaking-eq-sign
gh pr create --repo adafruit/circuitpython \
  --title "Fix sign of b2 in synthio PEAKING_EQ biquad" \
  --body-file <body extracted from the Body section above>
```
