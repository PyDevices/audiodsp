# Keeping the CircuitPython modules in step with upstream

Eight of audiodsp's modules are ports of CircuitPython's: `audiocore`,
`synthio`, `audiomixer`, `audiospeed`, `audiodelays`, `audiofilters`,
`audiofreeverb` and `audiomp3`. Upstream keeps fixing them, and every fix made
here that upstream has also made on its own is two copies of one change
waiting to disagree. This page is how the two stay in step: when to resync,
how, what the last one took, and every place this port differs from upstream
on purpose, so the next resync can tell intent from drift.

The reasons behind each difference, with their measurements, are in
[upstream-diff.md](upstream-diff.md). This page is the index into it.

## When

**Resync whenever the oracle pin moves.** The pin is `CIRCUITPYTHON_ORACLE`
at the repository root, and every CircuitPython-compatible gate is measured
against the release it names. Moving the pin and resyncing are one change: the
resync takes every fix upstream made between the old pin and the new one, and
the pin moves so the oracle can confirm them. A resync is never left for a bug
to turn up.

## How

1. Clone CircuitPython at the new tag into a scratch directory. Read it; never
   edit it.
2. List what upstream changed in the ported modules since the old pin:

   ```sh
   git log --oneline --no-merges OLD..NEW -- \
     shared-module/{audiocore,synthio,audiomixer,audiospeed,audiodelays,audiofilters,audiofreeverb,audiomp3} \
     shared-bindings/{audiocore,synthio,audiomixer,audiospeed,audiodelays,audiofilters,audiofreeverb,audiomp3} \
     lib/mp3
   ```

   Also diff the `lib/mp3` submodule commit against `DEPENDENCIES.lock`.
3. For each commit, find the same code here. A file under `src/<module>/`
   holds upstream's `shared-module` and `shared-bindings` files merged, with
   a `// --- from ...` line where each begins. Arithmetic that both bindings
   run lives in `src/shared/`, and the CPython target's copy of the module
   is `src/cpython/<module>.py` beside `_audiodsp.c`. A fix usually lands in
   two or three of those places.
4. Take each fix in upstream's own words where the code is still upstream's,
   so the next diff is short. Leave out anything that adds or changes public
   API, and anything in the list of deliberate differences below, and say
   which in the pull request.
5. Add a probe under `tests/parity/` whose output changes with each fix, and
   add it to `verify_dsp.py`. Run it against a build from before the change
   and see it fail.
6. Rebuild unix MicroPython and the CPython wheel from the change and run the
   gates: `verify_dsp.py` three ways (`--micropython`, `--circuitpython` with
   the oracle binary), the five stored-digest gates (`verify_acceptance`,
   `verify_effects`, `verify_streaming`, `verify_biquad`,
   `verify_mixdown_knee`) and the unit tests. Re-capture a stored digest only
   from the oracle's own output, and name each one that moved, with the
   reason, in the pull request.
7. Update the record below: what was taken, what was held back, and any
   difference that a fix retired or added.

## The last resync: 11.0.0-alpha.1 (audiodsp#220)

The pin moved from 10.3.0 to 11.0.0-alpha.1 with audiodsp#201, which took
6dddbda87 (buffer lengths and silence fills) and nothing else. audiodsp#220
took the rest:

- **synthio** (904e7a7a55). Ring modulation of two troughs is clamped to
  +32767 rather than flipping sign. A note past Nyquist for its waveform loop
  is not played, and a ring is held to Nyquist for its own loop rather than
  the main waveform's. A MIDI track cut off inside an event stops where the
  data ends. `MidiTrack` refuses a tempo of 0 and `Synthesizer` a sample rate
  of 0. `from_file` no longer frees the track it hands to the `MidiTrack`,
  which keeps parsing it as it plays.
- **synthio.from_file** (150230d3e4). Upstream reads through the stream
  protocol now, as this port always did, and its body is upstream's: it
  starts from the top of a file that was already part-read, and a short file
  is a `ValueError`.
- **audiocore** (b34aa34c19). The `sample_rate` setter every sample shares
  refuses a rate below 1.
- **audiocore.WaveFile** (cf227dc184, 34441b1af5, bd9b603c9c). Upstream's
  stream I/O replaces this port's own. An 8-bit file whose data ends inside a
  word is padded up to the next word, and a caller's buffer must be a multiple
  of 8 bytes.
- **audiomixer** (b34aa34c19). `play()` and `stop_voice()` check the voice
  index before narrowing it, so voice 256 is refused instead of playing on
  voice 0. The mono-into-stereo copy no longer writes past the end for an
  odd count.
- **audiomp3** (b34aa34c19, bf73aaf2af). An MP3 cut off mid-frame ends with
  `GET_BUFFER_DONE`, so a looping player starts it again. `rms_level` is
  worked out in `mp_float_t`. The swapped `stream_lseek` arguments were
  already fixed here in audiodsp#219, in upstream's form.

`tests/parity/cp11_fixes_probe.py` and `cp11_files_probe.py` cover them, and
every interpreter prints the oracle's bytes for both. One stored digest moved:
`midi_component`, whose one-byte track now reports its error at 1, where the
data ends, as the oracle does.

**Held back, because each adds or changes behaviour rather than fixing it:**

- `audiodelays.PitchShift.freeze` (9bef7b7606), a new property.
- Finalisers on every audio object (7f1de43480), which make the garbage
  collector call `deinit()`. See [No finalisers](#no-finalisers) below for
  why this port can't take them as they are.

Already here before the resync, because upstream took them from this port:
the oscillator's `>=` wrap (d02aed45a4), the full biquad reset (8a3deace5c),
the PEAKING_EQ `b2` sign (8fabdbbfb1), `Distortion(soft_clip=False)`
(cb2cdbb129) and a re-pressed finished note (e1b52a39af).

## Deliberate differences

Anything not listed here, or in a source comment that says why, is drift, and
a resync should take upstream's version. Each entry links to its reasons.

### Everywhere

- **Layout.** Each `src/<module>/<Class>.c` is upstream's `shared-module` and
  `shared-bindings` files for that class merged into one, docstrings (`//|`)
  dropped. The per-sample loops of the effects live in `src/shared/` so the
  CPython target runs the same C.
- **The CPython target** (`src/cpython/`) is a second implementation, held to
  the native bytes by `verify_dsp.py` rather than by sharing upstream's code.
- **`attr, cp_compat_attr`** on every type, because mainline MicroPython
  needs it to call a property
  ([why](upstream-diff.md#property-invocation-needs-an-explicit-attr-slot-tier-01-all-tiers-after)).
- **`m_malloc` for `m_malloc_without_collect`**, which mainline lacks.
- **Includes and declarations** that only make a merged file compile: a
  single `synthio_synth_t` typedef, dropped circular includes, explicit
  `<errno.h>`, `__attribute__((unused))`
  ([WASM fixes](upstream-diff.md#phase-8d-wasm-build-fixes)).

### The pump

`audiopump` pulls the graph on a thread with no interpreter, so the ported
modules carry what that needs. None of it changes a sample.

- Every state swap a Python call makes (`press`, a tap change, a new filter,
  `deinit()`) is taken under the pump lock, and nothing that can raise runs
  while it is held. `check_for_deinit` reads the flag under the lock and
  raises outside it
  ([the pull is a critical section](upstream-diff.md#the-pull-is-a-critical-section-and-the-funnel-stopped-raising-live-audio-path)).
- The pull funnel returns `GET_BUFFER_ERROR` and a fault code where upstream
  raises; `audiocore.get_buffer()` and `reset_buffer()` still raise.
- `WaveFile` and `MP3Decoder` refuse to be pulled on the pump thread, because
  they read through the VFS.
- `Synthesizer` walks its blocks as a list, never through the iterator
  protocol
  ([why](upstream-diff.md#synthesizer-walks-its-free-running-blocks-as-a-list-not-as-an-iterable)).
- `MultiTapDelay` builds new tap tables and swaps them in under the lock,
  where upstream resizes them in place
  ([why](upstream-diff.md#a-released-effect-lets-go-of-its-source-and-multitapdelay-swaps-its-taps-under-the-lock)).
- The protocol has an optional `sources` slot, so a graph can be walked
  without pulling it.

### deinit

- `deinit()` on the ported effects also lets go of `sample`, so a released
  node does not keep the chain behind it alive
  ([why](upstream-diff.md#a-released-effect-lets-go-of-its-source-and-multitapdelay-swaps-its-taps-under-the-lock)).

### No finalisers

Of the ported classes only `MP3Decoder` has a `__del__`. CircuitPython 11
gives one to every audio object (7f1de43480), so a collected object is
`deinit()`ed. Here the pump stops itself on a soft reset from a finaliser of
its own, and it relies on no finaliser tearing down a graph node first: a soft
reset runs every finaliser, reachable or not, so a node's `deinit()` could
free it under a pump that is still pulling (`src/audiopump/audiopump.c`,
"teardown"). Taking upstream's change needs that ordering solved first.

### By module

- **audiocore**: `get_buffer()` returns a byte view, and the oracle is
  patched to match
  ([why](upstream-diff.md#audiocoreget_buffer-returns-a-byte-view-circuitpython-patch)).
  A host reset keeps the frames a node has already taken
  ([why](upstream-diff.md#a-host-reset-keeps-the-frames-a-node-has-already-taken-audiodsp181)).
  `WaveFile` opens a path as well as a file.
- **synthio**: 64 voices where CircuitPython builds 14
  ([why](upstream-diff.md#the-voice-ceiling-is-64-where-circuitpython-builds-14-2026-09-03-recorded-2026-09-06)).
  `Note.filter` takes a tuple of `Biquad`s as a serial cascade
  ([extension](upstream-diff.md#extension-notefilter-accepts-a-serial-biquad-cascade-2026-09-01)).
  `from_file` opens a path as well as a file. `lfo_tick()` is always built.
- **audiomixer**: a reset rewinds the voices instead of stopping them
  ([why](upstream-diff.md#resetting-a-mixer-silenced-it-permanently-audioeffects-tier)).
  A voice at level 1.0 is a wire
  ([why](upstream-diff.md#a-voice-at-level-10-is-a-wire-audiodsp95)).
  A looping sample under one word is refused
  ([why](upstream-diff.md#audiomixer-a-looping-sample-under-one-word-is-refused-not-spun-on-audiodsp85)).
  Level and panning are always block inputs, and the ARM CMSIS intrinsics are
  the portable C
  ([why](upstream-diff.md#audiomixer-synthio-block-input-made-unconditional-arm-cmsis-dropped-tier-3)).
- **audiospeed**: the Q16 rate rounds and the phase carries across a source
  buffer
  ([rate](upstream-diff.md#audiospeed-the-q16-rate-rounds-here-upstream-truncates-audiodsp92),
  [phase](upstream-diff.md#audiospeed-the-phase-accumulator-crosses-a-source-buffer-upstreams-restarts-audiodsp91)).
- **audiodelays**: `Flanger` does not reproduce upstream's int32 overflow
  ([why](upstream-diff.md#audiodelaysflanger-we-do-not-reproduce-upstreams-int32-overflow-audiodsp76)).
  `MultiTapDelay` keeps its lanes after an odd-length buffer and never spins
  on an empty source
  ([why](upstream-diff.md#audiodelaysmultitapdelay-the-lanes-hold-and-a-pull-always-returns-audiodsp177)).
- **audiomp3**: a rewind clears the decoder's frame-to-frame state, so a
  looped second lap decodes like the first (audiodsp#219; 11.0.0-alpha.1
  keeps it). The allocator, three Windows-only fixes and the platform list
  are this port's
  ([allocator](upstream-diff.md#tier-5-audiomp3-allocator-wiring-diverges-from-upstream-both-are-correct),
  [Windows](upstream-diff.md#tier-5-audiomp3-three-windows-only-local-fixes-no-unix-impact),
  [platforms](upstream-diff.md#tier-5-audiomp3-on-cmakemcu-ports-mp3dechs-platform-list-and-a-qstr-extraction-blind-spot-phase-10)).

Kept as upstream has them, on purpose, though they look wrong (so a resync
should not "fix" them here): `audiofilters.Phaser`'s DC constant
([why](upstream-diff.md#audiofiltersphaser-holds-a-dc-constant-forever-audiodsp36-kept-verbatim))
and `Distortion` ignoring `drive` in OVERDRIVE mode
([why](upstream-diff.md#distortion-ignores-drive-in-overdrive-mode-upstream-worked-around)).
