# PR for adafruit/circuitpython #11268

Branch: `fix-biquad-reset` (one commit; the patch file beside this
document). Verified against upstream `main` at `d897c15f` on 2026-08-31.

## Title

```
Fix synthio_biquad_filter_reset() clearing only half the filter state
```

## Body

```markdown
Fixes #11268.

`shared-module/synthio/Biquad.h` defines the filter state as four
`int32_t`:

    typedef struct {
        int32_t x[2], y[2];
    } biquad_filter_state;

but `shared-module/synthio/Biquad.c` only clears the first two:

    void synthio_biquad_filter_reset(biquad_filter_state *st) {
        memset(&st->x, 0, 4 * sizeof(int16_t));
    }

The struct is 16 bytes; `4 * sizeof(int16_t)` is 8. `x[0]` and `x[1]` get
cleared, but `y[0]` and `y[1]` -- the feedback history -- do not.
`biquad_filter_sample()` reads all four members as `int32_t`, so the
`int16_t` sizing looks like a leftover from an earlier, narrower state
layout.

A biquad's `y` history is what the recursion runs on, so what survives a
"reset" is not a cosmetic detail: feeding a reset filter pure silence
produces a decaying tail of whatever it played before, and for a low-pass
with poles near the unit circle that tail starts close to full scale.
Both callers -- `audiofilters_filter_reset_buffer()` (called when an
`AudioOut` starts playback) and `synthio.Note`'s filter (re)initialisation
-- want the full reset.

### Repro

An 800 Hz low-pass fed a loud 200 Hz tone for four blocks, then
`reset_buffer()`, then silence. Peak of the first "silent" block, unix
coverage build of `main`:

|        | peak out (int16) |
|--------|-------------------|
| before | 28082 (-1.3 dBFS) |
| after  | 0                 |

Includes a regression test
(`tests/circuitpython/audiofilter_filter_reset_buffer.py`) with the same
scenario; it fails on current `main` and passes with the fix.
```

## Verification transcript (2026-08-31, upstream main d897c15f)

Before (unmodified `main`, unix coverage build):

```
$ ports/unix/build-coverage/micropython tests/circuitpython/audiofilter_filter_reset_buffer.py
peak after reset_buffer: 28082
```

After (same build, `memset(&st->x, 0, 4 * sizeof(int16_t))` -> `memset(st,
0, sizeof(*st))`):

```
peak after reset_buffer: 0
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

Note: the draft's "before" number (measured 2026-08-27) was 28072; today's
build reads 28082. Same bug, same order of magnitude of residual (a
1-in-32768 rounding difference somewhere upstream of this fix, not caused
by it); the PR body above carries today's number only.

Note: `common_hal_audiofilters_filter_play()` does not call
`audiofilters_filter_reset_buffer()` -- it resets the source only -- so a
plain `filter.play(other_sample)` deliberately carries the filter memory
over. That path is untouched; this fix is only for the path that means to
clear it (`reset_buffer`, and note (re)initialisation).

## Publish commands (Brad runs these; nothing has been pushed)

```sh
git clone https://github.com/adafruit/circuitpython.git
cd circuitpython
git checkout -b fix-biquad-reset
git am /path/to/audiodsp/docs/upstream-reports/prs/biquad-reset/0001-Fix-synthio_biquad_filter_reset-clearing-only-half-t.patch

gh repo fork adafruit/circuitpython --remote --remote-name fork
git push fork fix-biquad-reset
gh pr create --repo adafruit/circuitpython \
  --title "Fix synthio_biquad_filter_reset() clearing only half the filter state" \
  --body-file <body extracted from the Body section above>
```
