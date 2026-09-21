# PR for adafruit/circuitpython

No existing upstream issue -- this defect wasn't among the five filed
2026-08-28 (#11265-#11269). Following tannewt's stated preference for
small fixes ("please open a PR instead of an issue... we can just merge
it"), this goes straight to a PR with no issue to link.

Branch: `fix-note-repress-envelope-zero` (one commit; the patch file
beside this document). Verified against upstream `main` at `801d77a3`
on 2026-09-02.

## Title

```
synthio: Fix note dropped when re-pressed after decay
```

## Body

```markdown
A note released and then re-pressed after its envelope has run down to 0
is silently dropped. `synth.pressed` reports it as pressed, and then the
next render loses it.

`synthio_span_change_note()`'s fast path for a note still on its channel
re-enters ATTACK without touching `level`. At level 0 the render loop's
"note is truly finished" check reaps the channel before the envelope is
stepped, so the re-press never sounds.

Test included; fails on main, passes with the fix.
```

## Why it matters (not for the PR body -- context for us)

An instrument that models one strike as a couple of notes sharing a
trigger (body + click, say) presses more than one note per hit. A
note-off followed by a fast re-strike lands in this window often enough
to be audible as a missing layer on the repeat, not just a synthetic
corner case.

## Verification transcript (2026-09-02, upstream main 801d77a3)

### The bug, still present on current main

```
$ grep -n "level == 0\|truly finished\|synthio_span_change_note\|re-enter attack\|synthio_envelope_state_init\b" shared-module/synthio/__init__.c
128:static void synthio_envelope_state_init(synthio_envelope_state_t *state, synthio_envelope_definition_t *def) {
369:        if (synth->envelope_state[chan].level == 0) {
370:            // note is truly finished, but we only just noticed
497:bool synthio_span_change_note(synthio_synth_t *synth, mp_obj_t old_note, mp_obj_t new_note) {
500:        // note already playing, re-enter attack phase
510:            synthio_envelope_state_init(&synth->envelope_state[channel], synthio_synth_get_note_envelope(synth, new_note));
```

The fast path at line 497-503 (unfixed):

```c
bool synthio_span_change_note(synthio_synth_t *synth, mp_obj_t old_note, mp_obj_t new_note) {
    int channel;
    if (new_note != SYNTHIO_SILENCE && (channel = find_channel_with_note(synth, new_note)) != -1) {
        // note already playing, re-enter attack phase
        synth->envelope_state[channel].state = SYNTHIO_ENVELOPE_STATE_ATTACK;
        return true;
    }
```

`synthio_envelope_state_init()` (line 128-134) is what a press on an
already-reclaimed slot uses; it steps the envelope once, synchronously,
before the render loop ever looks -- which is why a normal first press
never trips the reaper. The re-press fast path above skips that.

### `synth.pressed` before the fix (unmodified main, unix coverage build)

```
$ ports/unix/build-coverage/micropython repro.py
after press: 2
click level right after release: 0.0 state: synthio.EnvelopeState.RELEASE
after release: 0
pressed right after re-press call: 2
after one more render: 1 [110.0]
```

(`repro.py`: two notes, 110 Hz with a 1 s release and 2000 Hz with a 0 s
release; press both, release both, re-press both. The 2000 Hz "click"
note is dropped -- `pressed` is 1, and only the 110 Hz note answers.)

### After the fix (same build, same script)

```
after press: 2
click level right after release: 0.0 state: synthio.EnvelopeState.RELEASE
after release: 0
pressed right after re-press call: 2
after one more render: 2 [110.0, 2000.0]
```

### The re-press now matches a genuine fresh press, step for step

`repro2.py` compares the envelope trajectory (state, level) over four
renders for (a) a brand-new `Note` pressed for the first time and
(b) the same shape of note released to level 0 and re-pressed, with the
fix applied:

```
pre-repress state/level: synthio.EnvelopeState.RELEASE 0.0
fresh press trajectory:   [('ATTACK', 0.2133), ('ATTACK', 0.32), ('ATTACK', 0.4266), ('ATTACK', 0.5333)]
re-press trajectory:      [('ATTACK', 0.2133), ('ATTACK', 0.32), ('ATTACK', 0.4266), ('ATTACK', 0.5333)]
match: True
```

### Regression test round-trip

```
$ MICROPY_MICROPYTHON=../ports/unix/build-coverage/micropython ./run-tests.py circuitpython/synthio_note_repress_after_decay.py
pass  circuitpython/synthio_note_repress_after_decay.py   (fixed binary)
```

Stashing the fix and rebuilding reproduces the unfixed `.exp` mismatch
(`pressed after one more render: 0`, `None 0.00` x3) -- fails on
unmodified `main`, passes with the fix.

Ran alongside its neighbors, same build:
`synthio_note_info.py`, `synthio_oscillator_loop_end.py`,
`audiofilter_filter_reset_buffer.py`, `synthio_biquad_peaking.py` all
still pass. `synthio_biquad.py` fails identically on unmodified `main`
in this WSL environment (full-precision float prints vs. the
checked-in `.exp`, unrelated to this change -- confirmed by stashing
the fix and re-running: same failure, same build).

### The alternative we tried and rejected: gating the reaper instead

Our first instinct was to leave the fast path alone and instead skip
reaping a level-0 note while its state is `ATTACK` (i.e. gate the
`level == 0` check in the render loop on `state != ATTACK`, rather than
fixing the press path). Implemented as a one-line experiment
(`synth->envelope_state[chan].level == 0 && ...state != SYNTHIO_ENVELOPE_STATE_ATTACK`)
and measured with the same `repro2.py` trajectory comparison:

```
pre-repress state/level: synthio.EnvelopeState.RELEASE 0.0
fresh press trajectory:   [('ATTACK', 0.2133), ('ATTACK', 0.32), ('ATTACK', 0.4266), ('ATTACK', 0.5333)]
re-press trajectory:      [('ATTACK', 0.1067), ('ATTACK', 0.2133), ('ATTACK', 0.32), ('ATTACK', 0.4266)]
match: False
```

It never resets `level`/`substep` the way `synthio_envelope_state_init()`
does, so it's missing the synchronous first step every other press gets
-- the re-pressed note comes in a full envelope step behind a fresh
press, permanently, for as long as it's held. Rejected; not part of
this patch, and not part of the PR body (only mentioned here in case a
reviewer suggests the same shortcut).

### Pre-commit

`pre-commit run --files shared-module/synthio/__init__.c
tests/circuitpython/synthio_note_repress_after_decay.py
tests/circuitpython/synthio_note_repress_after_decay.py.exp`:
end-of-file, trailing-whitespace, codespell, ruff, ruff-format all
passed. `Translations` fails in this environment for unrelated reasons
(`xgettext` and `polib` aren't installed) -- it emptied
`locale/circuitpython.pot`, reverted, not part of the commit.
`Formatting` (uncrustify 0.78.1, built from source, run against
upstream's `tools/uncrustify.cfg` which triggers several
deprecated-option warnings on this uncrustify version) ran clean on the
touched lines but also collapsed a space in an unrelated line 60 lines
away (`time * sample_rate` -> `time *sample_rate`); hand-reverted, not
part of the commit -- same tool quirk noted in earlier runs, always
diff after this hook.

### Build

`git clone --depth 50`, `python3 tools/ci_fetch_deps.py tests`,
`make -C mpy-cross -j8 && make -C ports/unix VARIANT=coverage -j8`.
Clean build both before and after the fix.

## Publish commands (Brad runs these; nothing has been pushed)

From a checkout with this branch (the scratch clone lives at
`/tmp/claude-1000/-home-brad-gh-pydevices/1139bed2-49e9-471e-8f3f-9ff13eab29f0/scratchpad/upstream/circuitpython`,
but a scratch dir may be gone -- `git am` the patch onto fresh `main`
instead):

```sh
git clone https://github.com/adafruit/circuitpython.git
cd circuitpython
git checkout -b fix-note-repress-envelope-zero
git am /path/to/audiodsp/docs/upstream-reports/prs/synthio-note-repress-zero-level/0001-Fix-synthio-dropping-a-note-re-pressed-after-its-env.patch

gh repo fork adafruit/circuitpython --remote --remote-name fork
git push fork fix-note-repress-envelope-zero
gh pr create --repo adafruit/circuitpython \
  --title "Fix synthio dropping a note re-pressed after its envelope hit 0" \
  --body-file <body extracted from the Body section above>
```

No issue exists to link with `Fixes #nnnn`. If Brad would rather file a
short issue first (house pattern: "a two-sentence issue plus the PR"),
two sentences that would do it:

> Pressing a note that's still on its channel but has already decayed
> to envelope level 0 is silently dropped instead of re-attacking --
> `synthio_span_change_note()`'s fast path re-enters ATTACK without
> resetting `level`, and the render loop reaps anything at level 0
> before stepping it. PR to follow.

## Independent mechanism check (Arthur, 2026-09-02 -- not for posting)

Checked because the drafting session cannot safely read the oracle C. Every
claim in the public body verified against upstream `main` at `801d77a3`,
fetched fresh from GitHub rather than read from our 10.2.1 pin.

- **Commit is real and on main.** `801d77a3f445`, 2026-09-01T23:40:23Z.
- **Every cited line is exact.** Reaper and its "note is truly finished"
  comment at 369-370; `synthio_span_change_note` at 497 with the fast path
  at 500-501; the fresh-press `synthio_envelope_state_init` call at 510;
  the init definition at 128.
- **The mechanism holds on main, not just on our pin.** The decisive
  property is loop order: the reap-and-render loop is at 363 and the
  envelope-advance loop at 403, so the `level == 0` test runs before
  anything steps the envelope, and the advance loop then skips the channel
  it just silenced. Re-press at level 0 is therefore deleted before it can
  sound.
- **The patch matches the fresh-press path it claims to reuse**, including
  `accum[channel] = 0`. Without that the re-pressed note would resume
  mid-waveform instead of from the start, which a fresh press does not do.
- **The rejected alternative is rejected for the right reason.** Gating the
  reaper on ATTACK instead would skip the synchronous
  `synthio_envelope_state_step(..., SYNTHIO_MAX_DUR)` that
  `synthio_envelope_state_init` performs at line 133, leaving a re-pressed
  note permanently one envelope step behind a fresh one. Confirmed on main.
  If a maintainer proposes that alternative, this is the answer.
- **The regression test follows upstream's own pattern.** `from audiocore
  import get_buffer` is gated behind `CIRCUITPY_AUDIOCORE_DEBUG`, so it was
  worth confirming: upstream's existing `synthio_note_info.py` opens with
  the identical two imports, so the test config enables it and the new test
  will run.

No corrections needed to the public body.

## Why this body is short (Arthur, 2026-09-02)

Trimmed from 163 words to 75 at Brad's direction, after measuring what
these maintainers actually merge. Median body length in merged PRs:
tannewt **16 words** (6 of 30 bodies empty), jepler **24** (2 of 30
empty), todbot 36, gamblor21 92, dhalbert 123. Restricted to bug-fix PRs
across tannewt/jepler/dhalbert the median is **43 words**; only three in
the sample exceed 200, two of them dhalbert on multi-bug BLE work. The
first draft was longer than every maintainer's median including the most
verbose one.

Their house style, from their own bodies: lead with the symptom or the
cause in plain prose, no section headings, no code blocks quoting
upstream's own source back at them, and no "Fix:" label -- the fix is the
diff. jepler's longest cause-and-effect fix (#10803) opens with one
sentence naming the regression and its consequence.

What was cut is not lost: the mechanism walk-through, the rejected
alternative and the verification transcript all stay in this file. They
just do not go in the box a maintainer reads on a phone.

## Correction before submission (Arthur, 2026-09-02)

The body originally opened "Press two notes, release both, re-press both:
`len(synth.pressed)` goes 2 -> 0 -> 1" -- the two-note shape of the
original bisection. The committed regression test is a **single-note**
minimal repro (`release_time=0`, press / render / release / render /
re-press). A maintainer running the test would have found it doing
something other than what the body described. Body now states what the
test actually does. The two-note asymmetry (one slot reaped and
reallocated, the other hit at level 0) is why the original finding showed
2 -> 0 -> 1, and is recorded in audiodsp#13 rather than the PR.

## SUBMITTED

[adafruit/circuitpython#11289](https://github.com/adafruit/circuitpython/pull/11289)
— opened 2026-09-02 from `bdbarnett:fix-note-repress-envelope-zero`,
branched from upstream main `801d77a3`, commit `e1b52a39`. No linked
issue, per tannewt's preference for small fixes going straight to a PR.
