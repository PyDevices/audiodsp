# PR for adafruit/circuitpython: audiospeed phase carry

Branch: `fix-speedchanger-phase-carry` (one commit, the patch beside this
document). From audiodsp#91 and the draft in
[speedchanger-phase-carry.md](../../speedchanger-phase-carry.md).

Independent of the [rate-rounding PR](../speedchanger-rate-rounding/PR.md):
different lines of the same file, each patch applies to a clean tree on its
own, and the two apply in either order. Verified against the `10.3.0` tag
(`d897c15f`) and the `main` tip (`268a168d`, 2026-09-17) — the two files
involved are byte-identical between the two.

## Title

```
audiospeed: carry the phase across source buffers
```

## Body

```markdown
The phase accumulator counts source frames since the stream began, but
`audiospeed_fetch_source_buffer` zeroes it at every new buffer, dropping the
remainder it was carrying. What a SpeedChanger renders then depends on how the
node above it chunks its output: at rate 1.5 over a ramp, a 4-frame source
gets 507 of 512 frames wrong where every block size should agree. Subtracting
the frames the replaced buffer held fixes it; `reset_buffer` still zeroes,
since the source restarts there.
```

## Verification transcript (2026-09-18) — not for posting

Bug still present on the `main` tip, cited from the file upstream serves
today (`gh api .../contents/shared-module/audiospeed/__init__.c?ref=main`,
`268a168d`, identical to `10.3.0`):

```
shared-module_audiospeed___init__.c:87:    audiospeed_reset_phase(&self->speed);
shared-module_audiospeed___init__.c:136:                src_index = 0; // phase was reset by fetch
shared-module_audiospeed___init__.c:158:                src_index = 0;
```

Built the `10.3.0` tree's unix coverage port (recipe in the rate-rounding
PR.md, "Building audiospeed on the coverage port") and ran `phase_carry.py`
beside this file: source frame n holds the value n, the rate is 1.5 — exact
in 16.16, so the rate bug is not in the way — and output frame n must be
source frame `(n * 3) // 2`.

Before (stock `10.3.0`):

```
$ ports/unix/build-coverage/micropython phase_carry.py
ideal           [0, 1, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16, 18, 19, 21, 22]
   4-frame src [0, 1, 3, 4, 5, 7, 8, 9, 11, 12, 13, 15, 16, 17, 19, 20]  507 of 512 frames wrong
 100-frame src [0, 1, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16, 18, 19, 21, 22]  411 of 512 frames wrong
 128-frame src [0, 1, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16, 18, 19, 21, 22]  426 of 512 frames wrong
```

After (same build, this patch applied):

```
$ ports/unix/build-coverage/micropython phase_carry.py
ideal           [0, 1, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16, 18, 19, 21, 22]
   4-frame src [0, 1, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16, 18, 19, 21, 22]  0 of 512 frames wrong
 100-frame src [0, 1, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16, 18, 19, 21, 22]  0 of 512 frames wrong
 128-frame src [0, 1, 3, 4, 6, 7, 9, 10, 12, 13, 15, 16, 18, 19, 21, 22]  0 of 512 frames wrong
```

The 4-frame case is the one to read by eye: it drops back a frame at every
boundary, so the staircase is not the one that was asked for. The 100- and
128-frame cases start correct and go wrong at their own first boundary.

`edge_cases.py` beside this file guards the two things the carry could have
broken — the stream still ending, and a carry larger than a whole source
buffer. Identical on both builds except the Resampler row, which is the rate
patch:

```
rate 0.3  RawSample(512) -> 1707 frames, done (512/0.3 = 1706 expected)
rate 1.0  RawSample(512) ->  512 frames, done (512/1.0 = 512 expected)
rate 1.5  RawSample(512) ->  342 frames, done (512/1.5 = 341 expected)
rate 6.0  RawSample(512) ->   86 frames, done (512/6.0 = 85 expected)
```

A build carrying only the rate patch renders `phase_carry.py` exactly as stock
does (507 / 411 / 426 frames wrong), so every number above belongs to this
patch alone.

Upstream's own `tests/circuitpython` suite: 20 pass, 3 skip, 34 fail, the same
34 with and without the patch (synthio and traceback tests that fail on stock
`10.3.0` in this environment for unrelated reasons).

`tools/codeformat.py` leaves the changed files untouched; `codespell` with
upstream's `.codespellrc` is clean.

Two details a reviewer may ask about, both in the commit message: a source
buffer shorter than one frame is now the end of the source (with the carry,
upstream's unconditional `src_index = 0` against a zero-frame buffer would
loop rather than read off the end of it), and the fetch loop runs until the
phase lands inside a buffer, because above rate 1.0 the carry can exceed a
whole buffer. `phase` stays `uint32_t`, so upstream's limit of 65535 source
frames per buffer is unchanged.

## Publish commands (Brad runs these; nothing has been pushed)

```sh
git clone https://github.com/adafruit/circuitpython.git
cd circuitpython
git checkout -b fix-speedchanger-phase-carry
git am /home/brad/gh/pydevices/audiodsp/docs/upstream-reports/prs/speedchanger-phase-carry/0001-audiospeed-carry-the-phase-across-source-buffers.patch

gh repo fork adafruit/circuitpython --remote --remote-name fork
git push fork fix-speedchanger-phase-carry
gh pr create --repo adafruit/circuitpython \
  --title "audiospeed: carry the phase across source buffers" \
  --body-file <the Body section above, fences stripped>
```
