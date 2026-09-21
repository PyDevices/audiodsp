# PR for adafruit/circuitpython #11266

Branch: `fix-dds-oscillator-off-by-one` (one commit; the patch file beside
this document). Verified against upstream `main` at `d897c15f` on
2026-08-31.

## Title

```
Fix synthio oscillator wrapping one sample late
```

## Body

```markdown
Fixes #11266.

`synth_note_into_buffer()` in `shared-module/synthio/__init__.c` wraps the
DDS accumulator on

    if (accum > lim) {
        accum = accum - lim + offset;
    }

but `lim` is the **exclusive** end of the waveform loop -- the readable
samples are `[offset, lim)`. Wrapping on `>` rather than `>=` lets the
accumulator land exactly on `lim`, and that iteration indexes
`waveform[waveform_length]`: one past the loop, and for the common case of a
note looping the whole table, one past the buffer itself. The ring-modulator
loop just below has the identical bug with `ring_waveform`.

Any note whose `dds_rate` divides `lim` hits the boundary on a schedule --
a table played at exactly one sample per output sample hits it every
`waveform_length` samples. When the loop covers the whole buffer, the read
is genuinely out of bounds, so **the same script with the same events does
not render the same audio twice**.

### Repro

A table with a loop end short of the buffer end, so the extra read stays
in-bounds and lands on a marker value instead of undefined memory
(`waveform_loop_end=256` of a 512-sample table, `frequency` set so the
oscillator advances exactly one table step per output sample):

Measured on a unix coverage build of `main`, four blocks of 256 samples
(1024 total), marker at index 256:

|                  | before                    | after |
|------------------|---------------------------|-------|
| marker hits      | `[255, 511, 767, 1023]`   | `[]`  |

Includes a regression test
(`tests/circuitpython/synthio_oscillator_loop_end.py`) built the same way;
it fails on current `main` and passes with the fix. The neighboring
biquad/filter tests that pass on unmodified `main` in the same environment
still pass.
```

## Verification transcript (2026-08-31, upstream main d897c15f)

Before (unmodified `main`, unix coverage build):

```
$ ports/unix/build-coverage/micropython tests/circuitpython/synthio_oscillator_loop_end.py
marker hits: [255, 511, 767, 1023]
```

After (same build with the fix, `>` -> `>=` in both loops):

```
marker hits: []
```

Regression test round-trip (run-tests.py, unix coverage build): fails on
the unfixed binary, passes on the fixed one.

Pre-commit: all hooks pass on the changed files (uncrustify 0.78.1, ruff,
codespell, end-of-file, trailing-whitespace, translations -- the
translations hook's rewrite of `locale/circuitpython.pot` was partial-run
churn and is not part of the commit).

Known-failing context: `synthio_biquad.py`, `audiofilter_filter_biquads.py`,
`audiofilter_filter_stereo.py`, `audiofilter_filter_stereo_biquads.py` fail
identically on unmodified `main` in this environment (float precision vs.
the checked-in `.exp` files) -- pre-existing, not caused by this change,
same pattern as the peaking-eq-sign PR. `audiofilter_filter.py` and the new
test pass.

Note: the draft's companion issue -- the pre-loop reduction (`accum %
lim + offset`) landing at or beyond `lim` when `waveform_loop_start` is
non-zero -- is not part of this patch. It is a separate, narrower case
(mid-note waveform changes) and keeping this PR to the one-character fix
that the repro demonstrates seemed better than folding in an unverified
second change; happy to split it into a follow-up if a maintainer wants it
addressed together.

## Publish commands (Brad runs these; nothing has been pushed)

```sh
git clone https://github.com/adafruit/circuitpython.git
cd circuitpython
git checkout -b fix-dds-oscillator-off-by-one
git am /path/to/audiodsp/docs/upstream-reports/prs/dds-oscillator-off-by-one/0001-Fix-synthio-oscillator-wrapping-one-sample-late.patch

gh repo fork adafruit/circuitpython --remote --remote-name fork
git push fork fix-dds-oscillator-off-by-one
gh pr create --repo adafruit/circuitpython \
  --title "Fix synthio oscillator wrapping one sample late" \
  --body-file <body extracted from the Body section above>
```
