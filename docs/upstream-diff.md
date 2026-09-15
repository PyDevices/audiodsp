# Deltas from upstream CircuitPython

## `audiodelays.Flanger`: we do not reproduce upstream's int32 overflow (audioif#76)

CircuitPython 10.3.0's `shared-module/audiodelays/Flanger.c:365` computes the
wet tap's interpolation in `int32_t`:

```c
int32_t wet = s0 + (((s1 - s0) * (int32_t)delay_frac) >> 16);
```

With the delay line holding adjacent samples at opposite rails, `s1 - s0`
reaches 65535 and `delay_frac` reaches 65535, so the product is 4.29e9 - past
INT32_MAX. Signed overflow, reachable on ordinary full-scale material.
Upstream used the widening cast eighteen lines earlier, at `:347`, for the
span times the triangle; it is absent here.

**This port widens it**, `src/shared/audioif_flanger.c`. So above about
±26000 alternating, our MicroPython build and CircuitPython 10.3.0 render
different bytes, and ours is the arithmetically correct one. Found by the
three-way `verify_dsp` run of 2026-09-09: 18 of 54 probe lines differed and
they were exactly the `rails` cases, every other case agreeing to the byte -
the signature of an overflow that only bites at the extremes.

`verify_dsp` carries a stated skip for `flanger_probe.py` on circuitpython
naming that issue, so the three-way is not red on a defect that is not ours.
An upstream report is drafted and held, per the rule that none goes out until
our own house is clean.

**Our CPython twin was never wrong**, and that is worth knowing rather than
comforting: it is written in Python, which has no int32 to overflow. A twin in
a language without fixed-width arithmetic cannot surface this class of defect,
so it is not evidence about it either way.

Running log of every place this port's behavior deliberately differs from
CircuitPython's, or needed a workspace-side fix that isn't a plain port.
Goal: keep this list short. Python-level API and behavior match CP unless
noted here.

Six of the entries below are upstream bugs rather than divergences, written up
in [upstream-reports/](upstream-reports/). **Five have been filed with
CircuitPython and four are merged** (see that directory's README for the
issue and PR numbers): peaking-eq-sign, dds-oscillator-off-by-one,
distortion-soft-clip-union and biquad-reset are fixed upstream;
biquad-band-edges remains open, awaiting maintainer appetite on a
three-option ask.

**Merged upstream is not the same as released.** `peaking-eq-sign`
(8fabdbbfb1) and `biquad-reset` (8a3deace5c) are on CircuitPython `main` and in
no tag -- 10.3.0 is the newest, and predates both. So this port is *ahead of*
the pinned oracle in exactly those two places, and each is a named departure
that `verify_dsp` records and that expires when upstream cuts a release
containing them. They are the reason `biquad_component_probe.py` compares only
the CPython twin against MicroPython and skips CircuitPython. **The numbers here were measured on this port and are not
upstream's** -- the reports carry figures measured on a build of upstream
`main`, which differ.

## Property invocation needs an explicit `attr` slot (tier 0/1, all tiers after)

CircuitPython's `MP_PROPERTY_GETTER`/`MP_PROPERTY_GETSET` (declared via
`py/objproperty.h`) only work in CircuitPython because CP patches its own
`py/runtime.c` (`mp_convert_member_lookup`, tagged `CIRCUITPY-CHANGE`) to
recognize a `mp_type_property` value sitting in a *native* type's
`locals_dict` and invoke its getter/setter. Mainline MicroPython's
`mp_convert_member_lookup` has no such case — confirmed by reading it
directly, not assumed. Discovered the hard way: a first pass at
`audiocore.RawSample` built cleanly but `sample_rate` returned the raw
property object (`<property>`) instead of its value, and assigning to it
raised `AttributeError`.

Can't carry CP's core patch (no edits to `micropython/`). Fix lives entirely
in this usermod: `cp_compat_attr` (`src/cp_compat/objproperty.c`)
reimplements the same lookup-and-invoke logic from usermod code, using
mainline's own supported extension point for this — a type's `attr` slot
(the same mechanism mainline's own `examples/usercmodule/cexample`
demonstrates for its `AdvancedTimer.seconds` property).

**Consequence for every future tier**: every `MP_DEFINE_CONST_OBJ_TYPE(...)`
call for a type that uses `MP_PROPERTY_GETTER`/`GETSET` in its
`locals_dict` must add `attr, cp_compat_attr` to that type's slot list, or
the property is silently unreadable/unwritable at runtime (it builds fine —
this doesn't show up as a compile error). `RawSample` and `WaveFile` do
this; use them as the reference when porting `Note`, `Synthesizer`, `Mixer`,
`MixerVoice`, and the effects types.

## WaveFile: portable stream I/O instead of direct FatFS calls (tier 1)

CircuitPython's `audiocore.WaveFile` reads its file via raw FatFS calls
(`f_read`/`f_lseek`/`f_tell` against a `pyb_file_obj_t`'s embedded `.fp`),
and its constructor hard-requires `mp_obj_is_type(arg, &mp_type_vfs_fat_fileio)`.
This only works when the mounted filesystem is FatFS — confirmed by testing
directly: even CircuitPython's own unix `coverage` build (`bin/circuitpython`
in this workspace, used as the parity oracle everywhere else) raises
`TypeError: file must be a file opened in byte mode` for `WaveFile(path)`
*and* `WaveFile(open(path, "rb"))`, because the unix port's `open()` returns
a POSIX-backed file object, not a `vfs_fat_fileio` one. WaveFile is
effectively FAT/MCU-only upstream, undocumented as such.

This port's `WaveFile` (`src/audiocore/WaveFile.c`) reads through
MicroPython's generic stream protocol instead (`mp_stream_read_exactly` +
an `ioctl(MP_STREAM_SEEK)`), which works over any VFS a port has mounted
(POSIX files on unix/windows, littlefs or FAT on mcu boards). The WAV chunk
parsing and the double-buffered refill/`get_buffer` state machine are an
unchanged, mechanical port — verified byte-for-byte against Python's own
`wave` module output for a generated test tone (`nbytes`/checksum/first-
and-last-32-bytes all matched exactly). Because CircuitPython's own unix
build can't open a WaveFile at all, a true CP-oracle diff for WaveFile PCM
output isn't possible on this port; if a FAT-mounted test fixture becomes
available later (real MCU or a FAT disk image), redo this comparison
directly against CP for full confidence — it's expected to match, since the
buffer/refill logic itself is unmodified, but it hasn't been checked yet
that way.

## `synthio.from_file`: same portable-stream deviation as WaveFile (tier 2)

`synthio.from_file()` (loads a `MidiTrack` from an SMF file) has the exact
same FatFS coupling as `audiocore.WaveFile` — same fix, same rationale: see
"WaveFile: portable stream I/O instead of direct FatFS calls" above. Only
this one function needed it in tier 2; `synthio.MidiTrack(buffer, tempo)`
itself (constructing from an in-memory buffer, no file I/O) is an unchanged
mechanical port. Verified functionally with a hand-built SMF fixture
(`from_file` accepts both a path string and an already-open file object);
no CP-oracle comparison possible here either, for the same reason as
WaveFile.

## `fpclassify()` triggers a false-positive `-Wfloat-conversion` here (tier 2)

`synthio/Math.c`'s `OP_MUL_DIV`/`OP_DIV_ADD`/`OP_ADD_DIV` cases used
upstream's `fpclassify(x) == FP_ZERO` to test for exact zero. This port's
unix build enables `-Wfloat-conversion -Werror` (`ports/unix/Makefile`),
and glibc's type-generic `fpclassify()` macro expands to a ternary across
`__fpclassifyf`/`__fpclassify`/`__fpclassifyl` (float/double/long double)
that GCC apparently still type-checks on the untaken branches, so it
flags a double truncating to float even though `mp_float_t` is `double`
here. Replaced with a direct `x == 0` comparison — exactly equivalent for
testing "is this float exactly zero" (`+0.0 == -0.0` is true in IEEE 754,
same outcome `fpclassify` would give), and avoids the compiler-specific
warning. If a future mcu toolchain doesn't hit this warning, the direct
comparison is still correct there, so no port-specific `#ifdef` needed.

## `audiomixer`: synthio block-input made unconditional, ARM CMSIS dropped (tier 3)

Two deviations, both mechanical:

- CircuitPython guards `MixerVoice.level`/`.panning` as `synthio_block_slot_t`
  behind `#if CIRCUITPY_SYNTHIO` (some CP boards omit synthio entirely) and
  falls back to plain scaled-uint16 fields otherwise. This port always
  builds synthio (tier 2), so the block-input path
  (`src/audiomixer/MixerVoice.h/.c`) is unconditional; the plain-float
  fallback fields and branches in `mix_down_one_voice`
  (`src/audiomixer/Mixer.c`) are dropped rather than carried as dead code.
- `Mixer.c`'s bit-twiddling helpers (`add16signed`, `mult16signed`,
  `tounsigned8/16`, `tosigned16`) have ARM Cortex-M4/M7 CMSIS DSP intrinsic
  fast paths (`__QADD16`, `__UADD8`, `__UADD16`, gated on
  `__ARM_ARCH_7EM__`) plus a top-of-file `#include "cmsis_compiler.h"`
  gated on `__arm__`. Dropped in favor of the portable C fallback
  unconditionally: numerically identical (verified via oracle-diff, not
  just assumed), and this port doesn't vendor CMSIS, so the include would
  be a dangling dependency on any ARM mcu build (phase 9) that doesn't
  happen to have it on the include path -- including non-DSP cores like
  Cortex-M0+ (rp2), where `__QADD16` isn't even a valid instruction.

Verified via oracle-diff against `bin/circuitpython` across every branch
`mix_down_one_voice`/`audiomixer_mixer_get_buffer` can take: mono source
into mono mixer, mono source upmixed into a stereo mixer with panning,
16-bit signed, 8-bit unsigned, single- and multi-voice mixdown (verbatim-
copy vs. add-mixed paths), looping, and `level`/`panning` driven by a
`synthio.LFO`/`synthio.Math` block input instead of a plain float -- all
byte-for-byte identical, including the `Mixer.reset_buffer` semantics
(stops every voice; call it before attaching voices, exactly like an
output device's `play()` would, not after -- documented here because a
first draft of the parity test called it in the wrong order and produced
an all-zero-but-still-oracle-matching result, which was CP's real
behavior for that call order, not a bug).

## Tier 4 effects: `audiospeed`, `audiofreeverb`, `audiofilters`, `audiodelays`

Nine types across four modules, all mechanical ports with the same fix
pattern as every prior tier (`m_malloc_without_collect` -> `m_malloc`,
`attr, cp_compat_attr` added to every type registration). Module-specific
notes:

- **`audiospeed.SpeedChanger`** has no oracle in the canonical
  `bin/circuitpython`: `CIRCUITPY_AUDIOSPEED` is gated per-port (only
  `ports/raspberrypi/mpconfigport.mk` sets it), and this workspace's unix
  coverage variant (`circuitpython/ports/unix/variants/coverage/mpconfigvariant.mk`)
  hand-lists its `SRC_C` files rather than using the `CIRCUITPY_AUDIOSPEED`
  Make-variable cascade, so simply passing the flag on the command line
  doesn't pull the module in. Verified anyway: built a *temporary* oracle
  by adding `shared-bindings/audiospeed/{__init__,SpeedChanger}.c` and
  `shared-module/audiospeed/{__init__,SpeedChanger}.c` to that variant file
  and rebuilding into a scratch `ports/unix/build-coverage-audiospeed/`
  directory (never touching the canonical `build-coverage/`), diffed
  byte-for-byte, then reverted the variant-file edit and deleted the
  scratch build -- the parent workspace's circuitpython checkout is back to
  its pre-existing local patch state, unmodified beyond that one-off test. Also needed the same
  `0.001`/`1000.0` -> `0`/`1000` literal-truncation fix as this port's
  `rate_to_fp` for `mp_arg_validate_obj_float_range`'s int-typed min/max
  (this is a *stock upstream* compile hazard under `-Wfloat-conversion
  -Werror`, not something introduced by the port -- confirmed by building
  CircuitPython's own unmodified `SpeedChanger.c` and hitting the same
  error; it has apparently never been caught because unix is the only
  build that enables that warning and unix never compiles this module by
  default).
- **`audiofreeverb.Freeverb`** ported unchanged, including upstream's own
  `combfitlers` identifier typo and the type's lowercase `MP_QSTR_freeverb`
  name (so `type(x).__name__` prints `"freeverb"` even though the class is
  `audiofreeverb.Freeverb`) -- both kept verbatim for parity, confirmed
  genuine upstream quirks (not transcription errors) by reading the
  original source directly.
- **`audiofilters.Distortion`** originally had two verbatim-kept upstream
  oddities; one was later reversed (see "Distortion soft_clip" below, phase
  8d) once it turned out to be architecture-dependent rather than a stable
  quirk. The one still kept verbatim: the unsigned-16-bit silence path's
  `memset(word_buffer, 32768, ...)`, which -- because `memset`'s fill value
  truncates to an `unsigned char` -- actually writes zero bytes, not the
  intended `0x8000` "quiet" level. `audiodelays.PitchShift` has the same
  `memset(..., 32768, ...)` quirk in its own silence path, plus a separate
  one: its per-sample `buf_offset` calculation ignores
  `single_channel_output` entirely (`channel == 1 || i % channel_count == 1`,
  unlike every sibling effect's
  `(single_channel_output && channel == 1) || (!single_channel_output && ...)`
  pattern) -- upstream's own inconsistency between effects, not a port bug.
- **`audiodelays.Chorus`** and **`audiodelays.PitchShift`** each define a
  custom `__exit__` that calls `common_hal_..._deinit(args[0])` directly
  instead of using the shared `default___exit___obj` method-dispatch
  helper every other type in this port uses -- kept verbatim as upstream's
  own micro-optimization for those two types specifically, not a port
  artifact.

All nine types (`SpeedChanger`, `Freeverb`, `Filter`, `Distortion`,
`Phaser`, `Chorus`, `Echo`, `MultiTapDelay`, `PitchShift`) verified
byte-for-byte against the oracle across mono/stereo, 8/16-bit
signed/unsigned, looping, block-input-driven (`synthio.LFO`) parameters,
mid-stream parameter changes, `stop()`/tail-drain behavior, and (for
`MultiTapDelay`) invalid-input error paths -- including the exact error
message text (a CRLF-vs-LF difference in how each interpreter's traceback
printer formats output was the only non-match found anywhere in this tier,
confirmed as a REPL-layer difference between CircuitPython and mainline
MicroPython, unrelated to any ported module).

## Phase 7: `synthtools` end-to-end acceptance test

`tests/vendor/synthtools` is todbot's
[`CircuitPython_SynthTools`](https://github.com/todbot/CircuitPython_SynthTools)
(MIT, commit recorded in `tests/vendor/SYNTHTOOLS_COMMIT.txt`), vendored
unmodified -- the `synthtools/` package only, not `examples/`/`tests/`/
`docs/`, since the acceptance script (`tests/parity/synthtools_acceptance.py`)
is this port's own, not upstream's. It drives `SubtractiveSynth` (dual
detuned oscillators, filter envelope, vibrato, pitch envelope, an
LFO-driven cutoff sweep, live mid-note parameter writes, a JSON patch
round-trip) and `BasslineSynth` (mono, decay-only filter envelope, glide)
through a real `synthio.Synthesizer` + `audiomixer.Mixer`, with an
`EffectsChain` wiring in `audiofilters.Filter` (via `tracking_filter()`),
`audiofilters.Distortion`, and `audiodelays.Echo` -- i.e. tiers 2/3/4 all
exercised together through an unmodified third-party engine, which is what
"full parity" is actually for. Runs unchanged on both interpreters; the
runner diffs stdout.

**Found and fixed a real port bug, not a verbatim-kept quirk:**
`src/synthio/__init__.h` defines `CIRCUITPY_SYNTHIO_MAX_CHANNELS` (max
concurrent `Note`s across one `Synthesizer`, not per-key) defaulting to 2 --
correctly mirroring CP's own conservative default
(`py/circuitpy_mpconfig.mk`). But this port's unix/windows build never
overrode it, while this workspace's CircuitPython oracle
(the parent workspace's circuitpython checkout, unix `coverage` variant,
`bin/circuitpython`) is itself built with `-DCIRCUITPY_SYNTHIO_MAX_CHANNELS=14`
(`ports/unix/variants/coverage/mpconfigvariant.mk`). At 2, anything past
the first two concurrently-alive `Note`s (e.g. two held notes on a
detuned/dual-oscillator patch -- already 4 `Note`s) silently truncates
voices, and every `synthio.LFO`/`synthio.Math` block nested under an
evicted voice desyncs from the oracle from that point on. Isolated via
binary search on `tests/parity/synthtools_acceptance.py`'s divergence: a
single note-lifecycle (attack through release, no overlap) matched
byte-for-byte on this port's *unpatched* build; 2 overlapping plain notes
(no modulation) also matched; 3+ overlapping notes, or 2 overlapping
notes on a detuned (2-oscillator) patch, diverged -- both cross the
2-concurrent-`Note` ceiling. Fixed by setting
`CFLAGS_USERMOD += -DCIRCUITPY_SYNTHIO_MAX_CHANNELS=14` in
`micropython.mk` (unix/windows only, matching the oracle's own build
choice so voice-stealing arithmetic lines up exactly, not just "enough
channels"); `micropython.cmake` (mcu/CMake ports) deliberately leaves the
header's conservative default of 2 alone, since the right value there is
a phase 10 (port matrix) RAM/polyphony tradeoff, not this one. Rebuilt
`bin/micropython` and reran the full tier 0-4 parity suite plus the LVGL
smoke test after the change: clean, no regressions.

**Test-harness-only, not a port deviation:** `synthtools/waves.py`'s
`random_phase_wave()` (used by `SubtractiveSynth._make_notes()` on every
note-on, by design -- see its docstring) calls `random.randint()` to pick
each oscillator's starting phase. MicroPython's and CircuitPython's
`random` modules implement different PRNGs, so the same script would
legitimately render different-but-valid PCM on each interpreter and defeat
a byte-exact diff. Neither interpreter's built-in `random` module allows
monkeypatching an attribute onto it directly (both raise `AttributeError`
on `random.randint = ...`), so `synthtools_acceptance.py` pre-seeds a tiny
deterministic-LCG substitute into `sys.modules["random"]` before importing
`synthtools` -- import picks up the substitute via the module cache, no
vendored file touched. This is a property of the test script, not of
`synthtools` or of the port.

Verified byte-for-byte against `bin/circuitpython`: the full acceptance
script's stdout (four `SubtractiveSynth` notes with a mid-run live
`filt_f`/`wave` change, a `pitch_bend()` sweep, a `Patch` JSON round-trip
reconstructing an equivalent synth, four `BasslineSynth` steps with
glide, `all_notes_off()`/`voice.stop()` tail-drain, and final `Mixer`
state) is identical after the `CIRCUITPY_SYNTHIO_MAX_CHANNELS` fix.

### The ceiling deviation is RETIRED (audioif#31, closed 2026-09-09)

There is no longer a ceiling deviation. The comparison build takes the ceiling
this port ships, which is what `docs/correctness-standard.md` requires of any
node CircuitPython also has: compare with CircuitPython *at the same
compile-time configuration*.

`CIRCUITPY_SYNTHIO_MAX_CHANNELS` is **64** on every build path (14 fits the drum
kits; 64 fits a ten-finger chord on the melodic library, whose instruments press
up to 9 `Note`s per key), and the pinned CircuitPython is now built at 64 too.
`SYNTHIO_MIX_DOWN_SCALE` is therefore 129 on both sides rather than 129 against
623, and `tests/parity/verify_mixdown_knee.py` is byte-identical to
CircuitPython **above** the knee as well as below. Nothing about that gate is
advisory any more.

What it took: the unix `coverage` variant hardcodes
`-DCIRCUITPY_SYNTHIO_MAX_CHANNELS=14` into CFLAGS rather than taking the `?=`
make variable the way `ports/raspberrypi/mpconfigport.mk` does, so overriding it
is a change to the build path and not a flag. `cmods/build_cp.sh` passes
`CP_CFLAGS_EXTRA` through for this, and the override must be `-U` then `-D`:
`-Werror` makes a conflicting redefinition an error, so "the last `-D` wins" is
not true here.

What the rebuild found, which is the argument for doing it: two behaviour
changes CircuitPython 10.3.0 made to `synthio` and `audiomixer` that this port
had not taken — the panning polarity flip and the zero-crossing loudness gate.
Both were quiet arithmetic edits rather than new nodes, every gate compared us
against our own past, and nothing else in the repository could have seen them.
That is the whole case for an oracle at our own configuration and version.

The old rule that the oracle must never be rebuilt is retired with this section;
`tests/test_voice_ceiling_consistency.py` carries the replacement and the
provenance of each re-pin.

## Tier 5 audiomp3: license

CircuitPython's `lib/mp3` (upstream `adafruit/Adafruit_MP3`, cloned as a
parent workspace sibling pinned to the same commit,
`aac02afd9f24d2ee930f650156654ab9211a306a`)
is the Helix fixed-point MP3 decoder, originally developed by RealNetworks in
2003. Every core decoder source file (`bitstream.c` through `statname.h`,
i.e. everything actually compiled -- not `Adafruit_MP3.cpp`/`.h`, Adafruit's
own Arduino wrapper, which neither CircuitPython nor this port uses) carries
a `RCSL 1.0/RPSL 1.0` SPDX-style header block, not MIT:

    Portions Copyright (c) 1995-2002 RealNetworks, Inc. All Rights Reserved.
    ... subject to the current version of the RealNetworks Public Source
    License Version 1.0 (the "RPSL") ... or ... the RealNetworks Community
    Source License Version 1.0 (the "RCSL") ...

RPSL 1.0 is an OSI-approved open-source license; the obligation it places is
on *modifications to the licensed files themselves* (publish them under
RPSL), not on other code that merely links against or calls into them --
this is exactly the same relationship a project has with any vendored
GPL-incompatible-but-OSI-approved C library, and it is precisely how
CircuitPython itself (MIT overall) carries this dependency: unmodified,
under its own original headers, no relicensing attempted, no separate
top-level LICENSE entry for it either (confirmed -- CircuitPython's own repo
has no `lib/mp3`-specific license documentation beyond the per-file
headers). This port does the same: the parent workspace's `mp3` sibling is
vendored verbatim (one local patch, see below, kept as narrow as possible
and documented inline), its RPSL/RCSL headers untouched, and this section
is the disclosure.
Its `mp3/examples/test.mp3` (Adafruit's own bundled test fixture, used for
oracle-diff verification below) ships alongside it, license unclear but
not carried into this repo's own source tree -- it's a test fixture read at
test time from the cloned sibling, not vendored content.

## Tier 5 audiomp3: three Windows-only local fixes, no unix impact

All three were found only when building/running this workspace's Windows
MicroPython target (`mp-windows`/mingw-w64) -- CircuitPython has no Windows
port, so none was ever reachable upstream:

- **the parent workspace's `mp3/src/assembly.h`** picks an MSVC-only inline-`__asm{}` code
  path (plus an MSVC-only `#pragma warning`) whenever `_WIN32` is defined
  and `_WIN32_WCE` isn't. mingw-w64 GCC also defines `_WIN32` (confirmed:
  `echo | x86_64-w64-mingw32-gcc -dM -E - | grep _WIN32` -> `#define _WIN32
  1`), so it hit that branch and failed to compile (`__asm { mov eax, x ...
  }` is not valid GCC syntax under any target). Local patch: added
  `&& !defined(__GNUC__)` to the branch's condition, falling through to the
  file's portable C implementation (`FASTABS`/`MULSHIFT32`/`CLZ` as plain C,
  no inline asm) -- the same path this port's unix build already takes,
  since `_WIN32` is never defined there. Patch is inline-commented in the
  file itself with a pointer back to this section.
- **This port's own `src/audiomp3/mp3_alloc.c`** (`mp3_alloc`/`mp3_free`,
  standing in for CP's `shared-module/audiomp3/__init__.c`) dropped
  upstream's `MP_WEAK` (`__attribute__((weak))`). Upstream needs it so a
  board can override the allocator via `CIRCUITPY_AUDIOMP3_USE_PORT_ALLOCATOR`;
  this port has no such opt-in, so there is never a second, strong
  definition anywhere in the link. `x86_64-w64-mingw32-ld` failed with
  "undefined reference to `mp3_alloc`/`mp3_free`" during `micropython.exe`
  linking even though `mp3_alloc.o` compiled cleanly and `nm` showed both
  symbols correctly emitted as PE weak externals (`.weak.mp3_alloc.` /
  `w mp3_alloc`) -- a lone weak definition with nothing else in the link
  strongly referencing it is a known mingw-w64/GNU-ld PE-COFF gap, distinct
  from ELF (this port's own unix build resolves the identical weak-only
  definition without issue). Dropping `MP_WEAK` entirely -- correct here
  since nothing in this port ever needs to override these two one-line
  wrappers -- fixed the windows link with no effect on unix.
- **`src/audiomp3/MP3Decoder.c`'s `stream_readable()`** (ported from CP's
  own `MP3Decoder.c`) calls `stream_p->ioctl(stream, MP_STREAM_POLL, ...)`
  to check whether the input stream has data ready before attempting a
  read. Found by running the phase 7/9 oracle-diff parity scripts under
  `bin/micropython.exe` for the first time (phase 8c) -- previously only
  verified on unix and against `bin/circuitpython`, neither of which
  exercises this path the same way. Mainline's own
  `extmod/vfs_posix_file.c` raises `NotImplementedError("poll on file not
  available on win32")` from `MP_STREAM_POLL` for a real POSIX-style file
  object (not a socket) on `_WIN32` -- not a bug in this port, a documented
  gap in mainline's own windows-port VFS ioctl implementation, confirmed by
  reading that file directly. Every successful frame decode from a real
  file calls this (via the synchronous `background_callback_add` stub --
  see `cp_compat/background_callback.h`), so `MP3Decoder` reading from an
  `open()`ed file was completely broken on `micropython.exe` before this
  fix: the very first `get_buffer()` past the first frame raised. Local fix:
  `stream_readable()` now has an `#ifdef _WIN32` branch that skips the poll
  call and returns `true` unconditionally, exactly the same fallback
  already used a few lines below for a stream with no `ioctl` slot at all --
  `mp3file_update_inbuf_always()`'s own non-blocking-read handling
  (`mp_is_nonblocking_error`) is what actually prevents a stall either way,
  so this is a false-availability-check removal, not a correctness change.
  Verified: `parity_mp3decoder.py`/`parity_mp3decoder2.py`, previously
  untested on windows, now pass byte-for-byte against `bin/circuitpython`
  under `micropython.exe`; unix behavior (and the `_WIN32` branch never
  taken there) is unaffected, confirmed by re-running the full tier 0-5
  parity suite plus the LVGL smoke test on unix after the change.

## Tier 5 audiomp3: allocator wiring diverges from upstream (both are correct)

Upstream's Make glue picks between two different `MPDEC_ALLOCATOR(x)`/
`MPDEC_FREE(x)` wirings for `lib/mp3/src/buffers.c` depending on build
variant: the unix `coverage` variant (this workspace's oracle,
`bin/circuitpython`) wires straight to `malloc(x)`/`free(x)`; every other
CP port wires through `mp3_alloc(x)`/`mp3_free(x)`
(`shared-module/audiomp3/__init__.c`, `MP_WEAK`, defaulting to
`m_malloc_maybe`/`m_free`). This port always uses the second (production)
path -- see docs/porting-plan.md, "Tier 5" -- since it changes only where
the Helix decoder's internal buffers come from (GC heap vs. the C heap),
never the rendered PCM, confirmed by the byte-exact oracle diffs below
against a `bin/circuitpython` that itself takes the *other* path: this is a
build-configuration choice invisible to output, not a parity-relevant
deviation. `mp3_free` itself also isn't a literal port of CP's version --
see the previous section and the `mp3_alloc.c` header comment for why it
uses `gc_free` rather than a direct `m_free(ptr)` call (mainline's `m_free`
needs an explicit size on this workspace's unix port, which `mp3_free`'s
callers never have in scope).

## Tier 5 audiomp3 verified against the oracle

`MP3Decoder` oracle-diffed byte-for-byte against `bin/circuitpython` using
the parent workspace's `mp3/examples/test.mp3` (Adafruit's own bundled fixture, ID3v2.3,
MPEG1 Layer III, 40 kbps CBR, 44.1 kHz stereo, ~12s): full-track decode via
`reset_buffer`/`get_buffer` (checksum + byte count identical), `rms_level`
and `samples_decoded` at multiple points, construction from a filename
string vs. an already-open binary stream vs. a caller-supplied pre-allocated
buffer (all three produce identical PCM), the `file` property's getter and
setter, explicit `open()`, the deinit guard, and error paths (`TypeError`
for a text-mode file, `RuntimeError("Failed to parse MP3 file")` for
non-MP3 data). One behavior was deliberately NOT "fixed" because it isn't
broken: calling `reset_buffer` again after a full decode to EOF and
decoding a second time produces a *different* total byte count than the
first pass (1048320 bytes first pass vs. 1078272 second, on the test
fixture) -- confirmed byte-for-byte identical on both interpreters,
including down to the exact discrepancy, so this is a genuine,
faithfully-reproduced characteristic of the real decoder/ID3-skip
interaction, not a port bug.

## Phase 8d: WASM build fixes

Rebuilding `micropython.mjs`/`.wasm` (the depth-1 `USER_C_MODULES` glob picks
up `audioif` automatically once built; it just hadn't been
rebuilt since the usermod landed) surfaced a batch of portability bugs,
none reachable on unix or windows before now:

- **the parent workspace's `mp3/src/mp3dec.h`** picks its fixed-point/asm path from a closed
  list of `(__GNUC__, arch)` combinations, `#error`ing on anything else --
  with an explicit `MP3DEC_GENERIC` escape hatch for exactly this case.
  wasm32 matches none of the listed architectures. Fixed in
  `micropython.mk`: `-DMP3DEC_GENERIC` added, but only when building for
  the `webassembly` port specifically (detected the same way
  the parent workspace's `wasmbridge/micropython.mk` detects it: `$(findstring
  /ports/webassembly,$(abspath $(CURDIR)))`) -- unix and windows both
  already match a named `__GNUC__`/arch branch and must keep using it, not
  silently fall back to the generic path.
- **`src/synthio/__init__.h`'s `synthio_synth_t`, and
  `src/synthio/Note.h`'s `synthio_bend_mode_t`** were each declared with
  two `typedef`s of the same name -- a forward declaration plus the real
  definition, exactly mirroring how CircuitPython spells the same thing
  across its separate `shared-bindings`/`shared-module` header pair. Two
  `typedef`s of an identical type are legal under GNU extensions/C11 (and
  never even co-occur in one CP translation unit, since CP never merges
  those two headers), but a hard error under emscripten's strict `-std=c99
  -Werror`, which this port's merged-into-one-file convention exposes for
  the first time. Fixed by dropping the second (redundant) `typedef` in
  each case -- the type is already complete by then either way.
- **`src/synthio/Biquad.c`** used `M_PI`, a glibc/mingw `<math.h>`
  extension, not ISO C99 -- present transitively on unix/windows, absent
  under emscripten's stricter libc. Fixed with a local `#ifndef M_PI
  #define M_PI ... #endif` guard.
- **`src/audiomixer/Mixer.c`'s `copy8lsb`/`copy8msb`** are genuinely dead
  code -- confirmed by reading CircuitPython's own `Mixer.c` directly, which
  defines the identical pair, also unused (only `copy16lsb`/`copy16msb` are
  called; these look like a leftover from a since-removed 8-bit-native
  mixdown path). Not a port bug, but unix/windows's compilers apparently
  don't enable `-Wunused-function` as an error here while emscripten's
  `-Wall -Werror` does. Fixed with `__attribute__((unused))` rather than
  deleting faithfully-ported (if currently unreachable) code.
- **`src/audiomp3/MP3Decoder.c`** needed an explicit `#include <errno.h>`
  for the bare `EINVAL` it uses (matching upstream, which also uses it
  bare rather than `MP_EINVAL`) -- glibc and mingw-w64 expose it
  transitively through `<sys/types.h>`/`<unistd.h>` already; emscripten's
  libc does not. Also needed an explicit `(mp_float_t)` cast on
  `common_hal_audiomp3_mp3file_get_rms_level(self)`'s `float` return value
  before `mp_obj_new_float()` (which takes `mp_float_t`, `double` on every
  build in this workspace) -- the implicit widening is fine under GCC's
  default warning set but trips `-Wdouble-promotion -Werror` under clang.

Full DSP parity suite (all of tiers 0-5, plus `synthtools_acceptance.py`)
re-run against the rebuilt wasm interpreter via a headless Node driver
(`loadMicroPython()` from `micropython.mjs`, since there is no
`micropython`-style CLI binary for this port) and diffed against
`bin/circuitpython`: byte-for-byte identical on every script except two
environment-only gaps, neither a DSP bug --
`parity_mp3decoder.py`/`parity_mp3decoder2.py` need a real file
(`mp3/examples/test.mp3`) on disk, which this ad hoc Node harness never
staged into wasm's virtual filesystem (already separately verified on
unix/windows against the same oracle); and `parity_multitapdelay.py`'s
error-path case shows a differently-formatted traceback (`PythonError:
Traceback ...` from the JS loader wrapping the exception, vs. the
interpreter's own native traceback text) -- the same category of
REPL/host-layer artifact as the CRLF-vs-LF difference documented for tier 4,
not a module bug. `synthtools_acceptance.py` (phase 7) also does not run
byte-for-byte under this harness: it computes its vendor path from
`__file__`, and `mp.runPythonAsync(source_string)` (this Node driver, since
there is no wasm-equivalent of a `micropython script.py` CLI invocation)
executes the script as a REPL-style source string with no `__file__`
bound, unlike the other three interpreters invoked with a real path
argument. Not a DSP issue -- every DSP-only script above ran and diffed
cleanly through the same harness -- just a limitation of this one ad hoc
test driver, left as a documented gap rather than built out further this
phase.

## Distortion soft_clip: verbatim-kept union type-pun turned out to be architecture-dependent (phase 8d)

Tier 4's original decision (see above) kept upstream's `args[ARG_soft_clip]
.u_obj` type-pun (reading an `mp_arg_val_t` union through the wrong member)
verbatim, on the grounds that it reproduced the oracle's output
byte-for-byte -- true, but only ever checked on x86-64 (this port's
unix/windows targets and CP's own unix `coverage` oracle, all the same
architecture and calling convention). Running the same parity script
(`parity_distortion.py`) against the newly-rebuilt wasm32 interpreter
(phase 8d) exposed the actual bug: constructing `Distortion()` with no
`soft_clip` argument at all (default `False`) reported `soft_clip == True`
on the wasm build, because the union-punned read happened to interpret
`args[ARG_soft_clip].u_obj`'s *default* value (a boxed `mp_obj_t`, not a
real bool) as truthy there, where x86-64's specific pointer-truncation
behavior for that same default value happened to read as falsy.

Fixed to `args[ARG_soft_clip].u_bool` (see `src/audiofilters/Distortion.c`)
rather than kept verbatim a second time, for two reasons: it is
unambiguously what upstream's own C function signature (`bool soft_clip`)
intends, and every *real* CircuitPython board is 32-bit ARM, not x86-64 --
so the byte-exact-on-unix result was already the architecture-unrepresentative
case, not evidence the quirk was safe to standardize on. Verified after the
fix: `soft_clip` now reads correctly (matching caller intent) and produces
*identical* checksums across all three of this port's targets (unix,
windows, wasm) for the same script -- a stronger three-architecture
consistency check than the tier 4 verification had access to at the time.
**Sharper than the phase-8d writeup, found 2026-08-27 while drafting the
upstream report**: this is not wasm-only, and "no unix impact" was too kind.
The default reads correctly everywhere because `mp_arg_parse_all()` copies the
*whole* union for a defaulted argument, and the static `{.u_bool = false}` is
zero-filled. A **supplied** argument only has its one `bool` byte written into
an uninitialised stack union, so `.u_obj` is a pointer made of one meaningful
byte and seven stale ones. On this port's own x86-64 oracle build,
`Distortion(soft_clip=False)` reads back **`True`** -- the one case a user
would reach for to get the hard curve is the one case that cannot be right
except by luck. Drafted:
`docs/upstream-reports/distortion-soft-clip-union.md`.

This is now a deliberate, documented divergence from the x86-64 CP oracle
for this one field (the oracle still exhibits the original bug); the
`memset(word_buffer, 32768, ...)` truncation quirk noted alongside it in
tier 4 was re-checked and is *not* similarly architecture-dependent (`memset`
truncating its fill value to `unsigned char` is guaranteed by the C standard
everywhere), so it remains kept verbatim.

## Tier 5 audiomp3 on CMake/mcu ports: mp3dec.h's platform list, and a QSTR-extraction blind spot (phase 10)

Wiring tier 5 into `micropython.cmake` (deferred at phase 8e, closed at
phase 10 -- see docs/porting-plan.md) surfaced two separate issues, one
upstream (Adafruit_MP3/Helix, vendored as the parent workspace's `mp3`) and one in mainline
MicroPython's own CMake glue (`py/mkrules.cmake`, `py/py.cmake`).

**`mp3dec.h`'s closed `(__GNUC__, arch)` platform list has no Xtensa
branch, not just no RISC-V branch.** The phase 8e writeup (building the
ESP32-P4) only identified RISC-V as the gap, since that was the one board
being built at the time. Actually wiring tier 5 for CMake ports generally
showed the real shape of the problem: the list covers `ARM`, `__ARMEL__`,
`__i386__`, `__amd64__`, `__AVR32_UC__`, and a couple of named
microcontrollers -- Xtensa (esp32/esp32s2/esp32s3, not just the RISC-V
esp32 variants) was never covered either, matching wasm32 for the same
reason: it's just not `-D`efined into the pinned Helix decoder's platform
list. So `-DMP3DEC_GENERIC` is needed for every esp32 target this
workspace builds, Xtensa and RISC-V alike; only rp2 (RP2040/RP2350, ARM
Cortex-M, `__GNUC__ && __ARMEL__`) matches natively. Gated in
`audioif/micropython.cmake` on ESP-IDF's own
`CONFIG_IDF_TARGET_ARCH_RISCV`/`CONFIG_IDF_TARGET_ARCH_XTENSA` sdkconfig
variables (the same ones `esp32_common.cmake` branches on for its own
`MICROPY_CROSS_FLAGS` selection), not a bespoke detection mechanism.

**QSTR extraction on ESP-IDF CMake builds does not see INTERFACE-library
compile definitions the way the real compile does.** Setting
`target_compile_definitions(usermod_mpaudio INTERFACE MP3DEC_GENERIC)`
alone was *not* sufficient, even though the real per-object compile picked
it up correctly (confirmed present in `build.ninja`'s `DEFINES` for every
mp3 `.o`). MicroPython's own `py/mkrules.cmake` runs a separate
QSTR-extraction preprocessing pass (`makeqstrdefs.py pp ... -E ...`,
producing `qstr.i.last`) that builds its own flag list from a raw
`get_target_property(${MICROPY_TARGET} COMPILE_DEFINITIONS)` call --
which, per ordinary CMake semantics, returns only that target's own
directly-set definitions, not ones contributed transitively by a linked
`INTERFACE` library several links down the chain
(`usermod_mpaudio` → `usermod` → `MICROPY_TARGET`). That pass still fully
preprocesses every source (it needs real macro expansion to find `MP_QSTR_`
tokens), so it hit `mp3dec.h`'s `#error No platform defined` directly,
independent of whether the actual object compile would have succeeded.
Confirmed by diffing the qstr pass's actual `gcc -E ...` invocation
against the real compile rule for the same file: `-DMP3DEC_GENERIC` present
in the latter's `DEFINES`, absent from the former's flags entirely.

MicroPython's own `py/py.cmake` already has a helper for exactly this
class of problem -- `micropy_gather_target_properties(targ)`, which reads
`INTERFACE_COMPILE_DEFINITIONS` for `INTERFACE_LIBRARY`-typed targets and
folds them into `MICROPY_CPP_DEF_EXTRA` (the same accumulator
`mkrules.cmake` appends into `MICROPY_CPP_DEF` before building
`MICROPY_CPP_FLAGS`). But `esp32_common.cmake` only calls it in a loop over
`__COMPONENT_NAMES_RESOLVED` -- registered ESP-IDF components -- and
`usermod`/`usermod_mpaudio` are plain CMake targets created inside the
*main* component's own CMakeLists.txt, not components in their own right,
so that loop never reaches them. This is a real, if narrow, gap in
mainline MicroPython's ESP-IDF CMake glue, not anything specific to this
usermod -- any usermod defining `INTERFACE`-scoped compile definitions
needed for QSTR-sensitive preprocessing on ESP-IDF would hit the same
blind spot.

Fixed on our side, without touching mainline files: append
`MP3DEC_GENERIC` directly to `MICROPY_CPP_DEF_EXTRA` from
`audioif/micropython.cmake` when the arch check fires. This
works because `usermod.cmake` (and our aggregator beneath it) is
`include()`d into the port's CMakeLists, not `add_subdirectory()`d -- plain
(non-cache) CMake variables set here are visible later when
`mkrules.cmake` reads `MICROPY_CPP_DEF_EXTRA`, in the same directory scope,
regardless of the gather-loop's component-only reach. Verified end to end
by rebuilding both mcu data points from a clean build directory: ESP32-P4
(`MP3DEC_GENERIC` branch) and RPI_PICO (native-ARM branch, exercising the
"don't need the define, don't break anything" path) both built clean with
tier 5 now included -- see docs/porting-plan.md phase 10 for the exact
before/after firmware sizes.

## `RawSample` verified against the oracle directly (tier 1, for calibration)

Unlike `WaveFile`, `audiocore.RawSample` needed no adaptation and diffs
byte-for-byte against `bin/circuitpython` (10.2.1) for construction,
`sample_rate`/`bits_per_sample`/`channel_count` properties (get and set),
the deinit guard, the context-manager protocol, and rendered PCM via
`get_buffer`/`reset_buffer`. This is the calibration case: when a tier's
oracle diff matches this cleanly, the port is source-faithful, not just
"looks right."

## The oscillator wraps its accumulator one sample late, and reads off the end of the waveform (instruments tier)

Upstream's DDS loop (`shared-module/synthio/__init__.c`, and the ring
modulator beside it) advances a fixed-point accumulator and wraps it with

```c
if (accum > lim) { accum = accum - lim + offset; }
int16_t idx = accum >> SYNTHIO_FREQUENCY_SHIFT;
out_buffer32[i] = waveform[idx];
```

`lim` is `waveform_end << SHIFT`, and `waveform_end` is an exclusive bound --
the samples a note may read are `[waveform_start, waveform_end)`. Wrapping on
`>` rather than `>=` lets the accumulator sit *exactly* on `lim`, so that
iteration indexes `waveform[waveform_end]`: one past the loop, and for the
common case of a note looping an entire table, one past the end of the buffer
itself. The read is out of bounds, and what it returns is whatever the
allocator happened to leave after the array.

This is not a rare edge. Any note whose `dds_rate` is an exact multiple of the
sample step lands on the boundary on a schedule -- the noise tables the drum
machines play at `sample_rate / 8192` advance exactly one sample per frame and
hit it every 8192 frames, and each voice hits it at its own offset. The
practical consequence is that a render is not reproducible: the same script,
same events, same interpreter produced different PCM depending on how the heap
happened to be laid out. Confirmed directly on CPython -- rendering one TR-909
sequence gave different output under `PYTHONMALLOC=default`, `malloc`, and
`debug`, and changed again if unrelated objects were allocated beforehand.

Drafted for upstream: `docs/upstream-reports/dds-oscillator-off-by-one.md`,
with a repro that keeps the errant read *inside* the buffer (a loop end short
of the buffer end) so it is deterministic. Still present on `main` 2026-08-27,
at four sites.

Fixed here (`audioif_oscillator_fill()` in `src/shared/audioif_synth_dsp.c`,
shared by the MicroPython usermod and the CPython extension, plus the
MicroPython ring-modulator loop in `src/synthio/__init__.c`) by wrapping on
`>=` and subtracting the loop span, and by reducing an out-of-range incoming
accumulator into `[offset, limit)` rather than into `[0, limit) + offset`.

Kept upstream's structure otherwise; this is a correctness fix, not a
behavioral redesign. It is a deliberate divergence from the oracle only where
the oracle's behavior is undefined: every committed parity fixture
(`verify_effects`, `verify_streaming`, `verify_acceptance`, and the CPython API
tests) still matches its recorded hash after the change, because those
fixtures' waveforms and rates never land on the boundary. Where the boundary
*is* hit, there is no oracle value to be faithful to -- upstream is reading
memory it does not own.

Note for anyone diffing instruments against `bin/circuitpython`: the oracle
build still has this bug, so CircuitPython's own renders of boundary-hitting
material remain sensitive to its heap layout. Instrument parity runs treat
CircuitPython as advisory for that reason; CPython and MicroPython (both of
which take the fix) are the enforced targets.

## `audiodynamics` and `audioroute`: not CircuitPython ports at all (dsp-nodes tier)

Every other module here is CircuitPython's, moved. These two are not: they come
from micropython-vst3's `vstaudio` usermod (`usermods/vstaudio/vstaudio_dsp.c`),
where its effects library's compressors, limiters, gates, de-essers and
parallel branches were built. CircuitPython has no equivalent and never had
one, so there is no oracle in the parent workspace's circuitpython checkout to diff against and nothing
in this section is an upstream deviation. What it records instead is where the
port differs from *its* original.

The DSP itself is unchanged, `float` working precision included -- doubles
would be a better filter and a different one. It lives in
`src/shared/audioif_dynamics.c` and `src/shared/audioif_splitter.c`, so the
MicroPython usermod, the CPython extension and the CircuitPython spike all run
the same arithmetic; the per-runtime code is only the loop that pulls the
source. `tests/parity/verify_dsp.py` holds all three to what the original
rendered, byte for byte, by compiling `vstaudio_dsp.c` itself -- unmodified,
straight out of the sibling checkout -- into a throwaway interpreter
(`tests/parity/build_vstaudio_oracle.sh`). The usermod that publishes those
types cannot be imported directly: it is the plugin sidecar, and it wants a
shared memory mapping that a VST host created.

Two deliberate changes:

- **`Splitter(source, taps=n)` accepts the tap count as a keyword.** The
  original was positional-only, which reads badly at the effects library's call
  sites.
- **A tap holds a real object reference to its Splitter**, not the raw C
  pointer the original stored. Handing a tap to a `Mixer` and dropping every
  other name for the Splitter is the ordinary case, not an unusual one, and the
  collector has to be able to see that the 32 KB ring is still in use.

Quirks kept on purpose, because the effects library is written around them:

- Neither node ever reports `GET_BUFFER_DONE`. A starved chain gets silence.
  Both sit in the middle of a live graph, which is still running.
- `Dynamics` hands out at most 256 frames per call, and carries leftover source
  frames across output blocks.
- `Dynamics.reset_buffer` drops the detector envelopes but keeps the sidechain
  filter's memory and the last reported gain reduction.
- An `attack_ms` so long that its coefficient rounds to zero silently gets the
  10 ms default instead. This is why `audioif_dynamics_config_finish()` is a
  separate call rather than part of the initial state.
- Writing past a laggard tap's cursor drags that cursor forward and drops what
  it never collected; the branch skips ahead rather than stalling the graph.
- `SplitterTap.reset_buffer` does nothing. The cursors belong to the Splitter,
  and the other branches are still reading against them.

Neither node has `deinit`/`__enter__`/`__exit__`, unlike the ported
CircuitPython effects around them. The originals had no lifecycle, and giving
one to three implementations to keep in step buys nothing the collector does
not already do.

## `audiomath`: audioif's own, with no ancestor anywhere (phase 9)

`audiodynamics` and `audioroute` above are at least *someone's* code moved.
`audiomath` is not: nothing in CircuitPython and nothing in
micropython-vst3's engine multiplies one audio stream by another, so there is
no oracle to diff against and this section records a new module rather than a
deviation.

`Multiply(source, modulator, mix=1.0)` writes `source * modulator`, blended
back against the untouched source by `mix`. That is ring modulation, and with
a modulator that does not cross zero it is amplitude modulation. The palette
could not do either:

- **`synthio` rings a *note*** against an oscillator. It reaches synthesized
  notes and nothing else -- not a microphone, not a sample, not the output of
  another effect.
- **An LFO-driven parameter updates once per block**, about 187 Hz at 48 kHz.
  `audioeffects.Tremolo` is exactly this effect inside that ceiling; a ring
  modulator wants hundreds of hertz and a carrier of a few kilohertz.

The arithmetic is `shared/audioif_multiply.c`, Q15 and stateless: the product
is `(a * b) >> 15`, blended `(dry * a + wet * product) >> 15`, clamped. The
two negative rails are the one product that lands outside `int16`, which is
what the clamp is for; `tests/parity/multiply_probe.py` drives it there
deliberately rather than assuming.

**The two inputs fail in opposite directions, on purpose.** A source that runs
dry gives silence, the way every other node in the palette does. A modulator
that is absent, or has stopped, lets the signal through **untouched** --
`audioif_multiply_passthrough_s16()`. This is the one place where "no input"
and "an input of zero" must not mean the same thing: a missing modulator that
muted the signal would make every dropout a hard gate.

A modulator is normally a short looping table, and looping is free here rather
than a feature: `audiocore.RawSample` returns `GET_BUFFER_DONE` with its whole
buffer every time it is asked, so pulling one repeatedly *is* the loop. The
carrier `audioeffects.RingMod` builds therefore holds a whole number of cycles
(`modulation.py`, `_carrier`), because a partial one would step the phase once
per table and buzz at the table rate.

`verify_dsp.py` covers it, but differently from its two neighbours: with no
oracle, the golden is captured from the port under CPython and what it proves
is cross-interpreter agreement and no accidental drift, not fidelity to
something older. All three interpreters render it identically.

The module gained a second class, `SubOctave`, in the effects program's
Phase 1 -- an analog octave divider, which is arithmetic on one stream
against its own zero crossings. See "`audiomath` gains `SubOctave`" below.

### `apply_cp_patches.sh` could not add a module to a tree it had already
### patched

Found while landing this one, and worth recording because it failed quietly.
`insert_block_after` skipped any file whose `audioif-cp begin` marker was
already there, so extending a block -- which is exactly what adding a module
does -- reached a fresh CircuitPython tree and no other. `CIRCUITPY_AUDIOMATH`
never landed, and the build then failed a long way from the cause. It now
rewrites the contents between the markers when they differ, and reports
`current` / `updated` / `patched` so the three cases are distinguishable.

## `audioecho`: a delay with a filter inside its feedback loop (phase 10)

`audiodelays.Echo` exists upstream, and its feedback path is `echo * decay`
and nothing else. Everything a delay is actually *named* after falls out of
what happens in that path, so without it there is one delay with a level
knob:

- **Tape.** Every pass through a tape machine loses top and bottom and
  softens. Filtering the delay's output once, after the fact, is not the same
  thing: it darkens the first repeat exactly as much as the tenth.
- **Analog / BBD.** The same, further. A bucket-brigade line is band-limited
  by construction and its clock drifts.
- **Ping-pong.** Repeats alternating between the speakers needs each
  channel's output fed into the *other* channel's line. Two delays panned
  hard apart, which is all the palette could do, gives repeats on both sides
  at once.

`audioecho.FeedbackDelay` puts a one-pole low-pass (`damping_hz`), a one-pole
high-pass (`cut_hz`), a cubic soft-clip (`loop_drive`), a per-sample delay
modulation (`wow_hz`/`wow_depth_ms`) and a cross-feed (`cross_feed`,
`input_pan`) in the loop. `shared/audioif_feedback_delay.c`, `float` working
precision to match `audioif_dynamics.c`.

**A new module rather than arguments on `Echo`, deliberately.** An argument
added to audioif's copy of a CircuitPython module would not exist on a stock
board, so a `TapeDelay` written against it would silently be a different
effect there -- the exact failure `apply_cp_patches.sh` exists to avoid. A
new module either installs whole or is absent and says so on import.

Two details worth recording:

- **The wow oscillator is a magic-circle resonator**, two states rotated by a
  constant each frame, not `sinf()`. Per-sample modulation is the point (an
  LFO-driven `delay_ms` updates once per block, about 187 Hz at 48 kHz, so it
  steps rather than glides and there is no doppler), and a library call per
  sample would not be affordable on the parts this has to run on.
- **`reset_buffer` really does drop everything**, unlike `audiodynamics`,
  which keeps its sidechain filter and last gain. A delay's whole state is
  audible: a chain restarted with the old repeats still in the line plays the
  previous take over the new one.

Verified by `tests/parity/feedback_delay_probe.py` through `verify_dsp.py`,
with no oracle -- the golden is captured from the port. That is a weaker
claim than the `audiodynamics` fixtures make, but a stricter cross-interpreter
one than anything else in the suite: the loop is recursive and runs in
`float`, so a one-ulp disagreement between two builds would be fed back and
amplified rather than staying one ulp. All three render it identically.

### `audioeffects.TapeDelay` was low-passing the dry signal

Found while rebuilding it, and it had been there since the class was written.
The tone filter sat *after* the delay node, and the delay node had already
blended dry with wet -- so the filter darkened the untouched signal along
with the repeats. At the class's own defaults, and worse at the low mixes the
soundtrack uses:

| tone through `TapeDelay(mix=0.14, tone_hz=3800)` | before | after |
|---|---|---|
| 1 kHz | -0.02 dB | -1.30 dB |
| 4.3 kHz | -4.28 dB | -1.30 dB |
| 10 kHz | **-19.26 dB** | -1.31 dB |
| 16 kHz | **-33.33 dB** | -1.31 dB |

("after" is the mix attenuation, flat across the band, which is what a delay
at 14% wet should cost.) Three racks in the soundtrack used it, so their
renders move -- that is the fix arriving, not a regression.

## `audioecho.FeedbackDelay` gains a shape table, a slew, wet AM and a loop pitch shift (effects Phase 1)

Additive, and all four default off, so a `FeedbackDelay` built the way it was
*is* what it was -- `tests/parity/feedback_delay_probe.py`'s hash is unchanged
across this phase, which is the check that says so. The new paths get their
own fixture (`feedback_delay_options_probe.py`) rather than joining that one,
because a probe's golden is one hash over its whole output: a case appended to
it would move the very number the additive claim rests on. That fixture's
first two cases render `feedback_delay_probe.py`'s `plain` line for line.

Asked for by the effects program (audioif#37, its Phase 1 palette work): the
Phase 0 survey fixed traits across the modulation, delay and pitch families
that no arrangement of the existing nodes reaches, and the program's vision
§6 permits additive options on audioif's *own* modules for exactly that.

- **`wow_shape`** replaces the built-in sine with one period of the caller's
  own, `int16` Q15, a power of two from 2 to 4096 samples; `None` is the sine
  again. The node's only modulation was `wow_depth_frames * wow_sine` added to
  the delay, and a bucket brigade's delay is its line length over twice its
  clock -- so the Small Clone's *triangle on the clock* arrives as a
  reciprocal on the delay, which no sine is. The magic-circle pair carries no
  phase index to look a table up with, so the table gets a Q32 accumulator
  beside it, stepped from the same `wow_hz`. The pair keeps turning either
  way: `wow_am_depth` reads it, and stopping it would move the default path's
  arithmetic. The table is *borrowed*, not copied -- the bindings hold the
  object (`self->wow_shape` under MicroPython and CircuitPython, an open
  `Py_buffer` under CPython) because the config only keeps a pointer.
- **`delay_slew`** walks the read head to a new `delay_ms` instead of jumping
  to it, in delay-seconds per second -- a dimensionless rate, so it is the
  same number at every sample rate. `delay_ms` is a plain float read
  unsmoothed at the top of the loop, so before this a Time move landed in one
  jump on a block boundary: no pitch bend, and a discontinuity. A constant
  slew rate is a constant pitch offset for exactly as long as the move lasts,
  which is what an Echoplex's capstan and a DM-2's clock both do.
- **`wow_am_depth`** (0..1) puts the wow oscillator on the wet gain as well as
  on the delay. Tape and bucket-brigade level wobble is a loss, so this dips
  to `1 - depth` and returns to unity and never boosts; it is on the output
  only, so it cannot change what the feedback path does.
- **`loop_semitones`** (-24..+24) pitch-shifts the line read *inside* the
  loop, two taps half a `loop_window_ms` window apart under a triangular
  crossfade whose two halves sum to exactly one. Every pass rises again, which
  is what a shimmer is; `audiodelays.PitchShift` chained after a delay shifts
  the *sum* of the repeats once, not each repeat one more time.
  `loop_window_ms` defaults to 25 ms and is capped at a quarter of the line:
  it is the grain rate traded against smear, and it also bounds how far the
  read head may wander from the delay it was asked for.

  **A shifted loop repeats half a window later than an unshifted one**, and
  a class that reports its own timing has to account for it. The two taps sit
  at `delay + turn * window` and `delay + (turn + 0.5) * window` under gains
  that sum to one, so their weighted mean is `delay + window / 2` at every
  point of the cycle -- constant, not drifting. Measured with a single-sample
  click at 48 kHz, `delay_ms=200`, the shift on: the repeat lands at
  **212.49 ms** with a 25 ms window and **229.98 ms** with a 60 ms one,
  against 200.00 ms with the shift off. The consequence to state plainly:
  turning `loop_semitones` on or off *mid-stream* steps the read position by
  that half window, which is the same class of discontinuity `delay_slew`
  exists to remove from `delay_ms` and is not smoothed here. Set it when the
  node is built, or between takes.

**The option enum is appended, never renumbered.** `src/cpython/audioecho.py`
maps option names to those integers and `_audioif.c` range-checks against the
last one, so inserting an option would silently change what an already
installed wheel configures.

**Off means the same floats in the same order**, not the same floats times
one. With no table the offset is the expression it always was; with no slew
the read head is `config->delay_frames` itself, assigned rather than
computed; and both new gains are branched around rather than multiplied by
1.0. The one structural change to the default path is that the single line
read moved into a `tap_for()`/`read_tap()` pair the shifter's two taps share
-- the same operations on the same operands, which the unchanged hash
confirms.

What the four do, measured on the CPython target at 48 kHz rather than
asserted:

| Option | Measurement | Result |
|---|---|---|
| `delay_slew` | 200 -> 100.4 ms at 0.99175, pitch while travelling | **+1192.9 cents** (the law says +1192.8), +0.00 cents after arrival |
| `delay_slew` | first difference across the block boundary carrying the step | **1559**, against 1566 in steady state -- and **15960** with the slew off |
| `wow_shape` | a flat table at depth 4 ms vs. moving `delay_ms` by 4 ms | peak difference **0.0** |
| `wow_shape` | 256-point ramp, 2 Hz, depth 10 ms: d(delay)/dt over the middle half | mean **+0.04000**, spread **0.00219** (the sine's spread is 0.12966) |
| `wow_am_depth` | envelope min/max at depth 0 / 0.25 / 0.5 / 1.0 | **1.0000 / 0.7499 / 0.5000 / 0.0000**, against a law of 1-depth |
| `loop_semitones` | a 500 Hz burst, first three passes at +12 / -12 / +7 st | **1000, 2000, 4000** / 250, 124, 60 (ideal 250, 125, 62.5) / 749, 1118, 1700 (ideal 749, 1122, 1682) |

One cost worth stating: `powf` is called once, at configure time, to turn
semitones into a ratio -- the same shape as the `expf` and `sinf` the node
already calls there. The desktop targets and the parity oracle share a libm
so the goldens are safe, but a board's newlib could round that ratio
differently in the last bit, which would show as a different phase step and
so a different digest. Nothing per sample is a library call.

## `audiodynamics` gains lookahead and true-peak detection (phase 11)

Additive, and both default off, so a `Dynamics` built the way the original was
*is* the original -- `tests/parity/dynamics_probe.py`'s hash is unchanged
across this phase, which is the check that says so. The new paths get their
own fixture (`dynamics_extras_probe.py`) rather than joining that one, because
that one is held against `vstaudio_dsp.c` compiled unmodified and may only use
forms the original accepts.

- **`lookahead_ms`** holds the audio back while the detector reads ahead of
  it, so the gain is already down when the transient arrives rather than a
  fraction of a millisecond after it. Without it, brickwall limiting always
  overshoots the first cycle of every attack. Capped at 50 ms: it is latency
  the whole chain pays, and past a few milliseconds a limiter stops sounding
  like a limiter and starts sounding like it is ducking before the note.
- **`true_peak`** adds the level *between* samples to what the detector sees,
  by four-point half-band interpolation of the midpoint. A signal can pass
  through the ceiling between one sample and the next with no sample over it,
  and a converter downstream reproduces that peak; sample-peak limiting cannot
  see it at all. This is an estimate, not ITU-R BS.1770 true-peak metering,
  which oversamples by four -- it is the cheapest version worth having.

**The lookahead buffer is allocated by the bindings, not the DSP**, and only
when someone asks for one. 50 ms of stereo is 9.6 KB and `audioeffects` builds
nine `Dynamics` instances, so an unconditional buffer would cost 86 KB for a
feature almost nothing uses. `audioif_dynamics_lookahead_frames()` tells a
binding how much to hand over; the DSP uses whatever it has, so a binding that
allocates nothing gets no lookahead rather than reading off the end of one.

One deliberate difference from the node's documented reset behaviour: the
sidechain filter's memory and the last reported gain reduction still survive
`reset_buffer`, but **what is in the lookahead buffer does not**. That is
audio in flight, and a chain restarted with the previous take still queued
would play it.

## `audiodynamics` gains twenty-one options and an external key (effects program)

Additive, every one of them default-off, and the check that says so is inside
the probe rather than beside it: `tests/parity/dynamics_options_probe.py`'s
first case sets none of them and reproduces `dynamics_probe.py`'s `compress`
case line for line, on the same source at the same settings. That probe is
held against `vstaudio_dsp.c` compiled unmodified and its hash is unchanged
across this work; the new paths get their own fixture, with no oracle, because
the original has no ancestor for any of them.

The effects program's dossiers put eleven asks on this one node
([audioif#38](https://github.com/PyDevices/audioif/issues/38)). Each is a
fixed trait of a named circuit that the shipped knobs cannot reach by tuning,
and the numbers below were measured through the built CPython extension, one
frame at a time, so the gain traces are at sample rate.

**The detector, and what it listens to.**

- **`detector="rms"`**, with **`rms_ms`** (10 ms unset), runs a one-pole mean
  square and roots it, instead of the largest rectified sample across
  channels. A sine and a square of equal RMS 15 dB below threshold get gains
  0.68 dB apart from the peak detector and 0.00 dB apart from this one.
- **`feedback_detector=True`** points the detector at the frame the node last
  put out rather than at this frame's input: the side chain tapped after the
  gain cell, which is what a 1176, an LA-2A and a Fairchild all do. The gain
  still lands on the audio. Without it the feedback arm of a paired
  feed-forward/feedback measurement cannot be built at all, so three of the
  four compressor characters were feed-forward by construction.
- **`key(sample)`** feeds the detector from a different stream entirely.
  In C that is a new entry point, `audioif_dynamics_process_s16_key`; the old
  `audioif_dynamics_process_s16` forwards to it with a NULL key, so nothing
  that calls it changes. A key that runs dry starves the node the way an
  absent source does - silence, never a short block, and never a detector
  that quietly reverts to the audio. Measured: a gate over a quiet 220 Hz
  tone sits at -60.00 dB for twelve blocks unkeyed, and opens to 0.00 dB on
  block six when a keyed burst starts.
- **`true_peak`** was a flag. It is a level now: 1 is the four-point half-band
  midpoint estimate it always selected, 2 a 4x polyphase reconstruction -
  four phases of twelve taps, an order-48 FIR, the shape ITU-R BS.1770
  Annex 2 specifies for true-peak metering. **The taps are not the
  Recommendation's table.** They are a Blackman-windowed sinc, generated once
  on CPython and written into `audioif_dynamics.c` as literals, so no target
  recomputes them through its own libm and a board's single-precision build
  reads the same numbers the desktop does. Measured against a worst-phase
  f_s/4 full-scale tone at sixteen phases, error against the tone's true
  peak: off -3.01..+0.00 dB, 1 -1.24..+0.00 dB, 2 -0.17..+0.42 dB. The
  half-band estimate's own comment already conceded it does not pretend to be
  true-peak metering; this is the version that does, and it errs high, which
  for a limiter's ceiling is the safe direction.
- **`sidechain_lp_hz`** closes the top of the key band `sidechain_hz` opened
  the bottom of, and **`sidechain_poles=2`** cascades a second pole through
  both ends. Measured below a 2500 Hz corner: 5.29 dB/octave at one pole,
  10.31 dB/octave at two.
- **`key_listen=True`** puts the detector's own signal on the output instead
  of the audio - a gate's Key Listen switch. With a 4 kHz key high-pass over
  a 220 Hz tone the output peaks at 125 against the ordinary output's 3000.

**The gain computer.**

- **`depth_db`** replaces the expander's fixed -60 dB (`:185`) and the gate's
  fixed -80 dB (`:192`). A gate held closed settles at -0.00, -20.00, -40.00,
  -60.00 and -80.00 dB for those five settings. **Positive means unset**, and
  unset is those literals: a depth is an attenuation, so no usable setting is
  above 0 dB, and 0.0 had to stay available as a real value.
- **`hold_ms`**, with **`hysteresis_db`**, swaps the gate's memoryless gain
  computer for a closed/attack/hold/decay machine on the same peak level.
  Once ATTACK is entered it runs to full open whatever the key does next, and
  a crossing during DECAY re-enters ATTACK from wherever the gain got to -
  the trigger state a pure function of the instantaneous envelope cannot
  have. Measured on a 1 ms burst under a 200 ms attack: without hold the gate
  never leaves -80.00 dB; with `hold_ms=20` it runs to -0.44 dB and stays
  within 0.5 dB of open for 23.5 ms. The thresholds are precomputed linear,
  so this path skips the `logf` at `:307`; `gain_reduction_db` still costs
  one `gain_to_db`.
- **`relative_threshold=True`** drives the computer with the side-chained
  level minus the full-band level instead of an absolute overshoot, so a
  fixed spectral balance gets the same reduction wherever it sits in the
  level range. A fixed 200 Hz + 6 kHz balance at -6, -20 and -40 dBFS spreads
  10.57 dB with it off and 0.00 dB with it on.
- **`program_attack=True`** scales the attack coefficient by the **square
  root** of how far the level is over the threshold. Two things that did not
  work and why: measured against the *envelope*, the ratio is enormous out of
  silence and every attack collapses to one sample; scaled by the overshoot
  itself, 20 dB over lands 6.2x faster than 10 dB over, past every dossier's
  band. The root gives time-to-63% of 1.167 ms at 10 dB over and 0.333 ms at
  20 dB over, a ratio of 3.50 where the DeEsser dossier asks 3.3 within 25%.

**The transient shaper.**

- **`transient_fast_attack_ms`**, **`transient_fast_release_ms`**,
  **`transient_slow_attack_ms`** and **`transient_slow_release_ms`** are the
  four detector time constants that were compiled-in literals evaluated at
  process entry (`:215`, `:217`, `:219`, `:221`), under a comment saying they
  were fixed by the shaper's design. They are config fields now, defaulted to
  those same literals, so a node that leaves them alone computes exactly what
  it computed before.
- **`slow_hold_ms`** makes the slow envelope a peak-hold rather than a
  follower, so the sustain difference grows monotonically through a note's
  decay instead of peaking early and falling away with it. On a 220 Hz note
  decaying at 10 dB/s, gain read once per block: the stock shaper reaches
  -0.87 dB at 200 ms and the peak-hold reaches -9.09 dB with no block coming
  back up by as much as 0.25 dB.
- **`transient_dual=True`** runs a second, slower envelope pair
  (**`sustain_fast_attack_ms`** and its three siblings, defaulting to 1, 200,
  25 and 1200 ms) for the sustain section, and applies both differences at
  once rather than selecting one by the sign of the first.
  **The peak-hold goes on whichever pair drives the sustain section** - the
  only one there is, or the second when `transient_dual` gives sustain its
  own. Held on the first pair while a second exists it costs the attack gain
  for nothing, because a held slow envelope is never below the fast one:
  measured +0.00 dB of attack gain that way against +4.91 dB the right way,
  with the sustain still reaching -6.99 dB at 200 ms.

**What a reset drops.** `audioif_dynamics_clear_extras()` is called by both
`state_init` and `reset`, and clears the RMS envelope, the stored output the
feedback detector reads, the full-band envelope, the second transient pair,
the slow peak-hold, the gate machine's stage/gain/hold and the 4x
reconstruction's history. The **side-chain filters are deliberately not in
it** - not the second high-pass pole and not either low-pass pole - because
the original keeps its side-chain filter memory and its last reported gain
reduction across a reset, and these are the same kind of thing.

**Cost.** None of it is paid by a node that does not ask. The per-sample
additions sit behind branches on config fields that are zero or false by
default, and the two expensive ones are opt-in by construction: the 4x
reconstruction is 48 multiplies per channel per sample, and the second
side-chain pole is one multiply-add per channel per sample per end of the
band. The four transient coefficients and the second pair's four are computed
once per block, as the originals were.

## `audiocore.get_buffer` returns a byte view (CircuitPython patch)

The one place `apply_cp_patches.sh` changes code CircuitPython already had,
rather than adding to it. Upstream returns a `memoryview` typed by the sample's
width, so `len()` counts samples while the C protocol's `buffer_length` counts
bytes -- every byte calculation downstream is then wrong by the sample width, a
silent 2x for ordinary 16-bit audio. This port's own `audiocore.get_buffer`
returns a byte view (audioif 413d87a), and the parity probes compare `len()`
and slices across all three interpreters, so the oracle has to agree.

The rewrite lives in `src/circuitpython_spike/apply_replacements.py` with the
upstream text it replaces. It is idempotent, and it fails loudly rather than
quietly if neither its marker nor the original text is present -- that means
the file moved upstream and a person should re-read it.

## Resetting a Mixer silenced it, permanently (audioeffects tier)

`audiomixer.Mixer`'s `reset_buffer` stops every voice upstream:

```c
for (uint8_t i = 0; i < self->voice_count; i++) {
    common_hal_audiomixer_mixervoice_stop(self->voice[i]);   // sample = NULL
}
```

Every other source in the stack treats `reset_buffer` as "rewind to the
beginning". This one drops what was playing and never picks it up again, so a
Mixer that has been reset renders zeros for the rest of its life.

That is not a corner case, because *pulling from a source resets it first*.
`Filter.play(sample)`, `Echo.play(sample)`, every effect's `play()` and every
output's, all call `reset_buffer` on what they were handed. So

```python
mixer.voice[0].play(source)
effect.play(mixer)          # <- silences the mixer here
```

renders silence, and always has. It went unnoticed because a Mixer is normally
the last node before the output, and because `MixerVoice.play()` re-primes the
voice, so a voice started *after* the reset works fine — which is how every
example is written.

Found while moving micropython-vst3's effects library into `audioeffects`:
`ParametricEQ` sums its boost branches in a Mixer and then chains the cut
sections after it, so any curve with both a boost and a cut was silent.
Confirmed against `bin/circuitpython` directly — the oracle does the same
thing.

Fixed here by rewinding instead of stopping: a new
`common_hal_audiomixer_mixervoice_reset()` (`src/audiomixer/MixerVoice.c`,
and `MixerVoice.reset()` in `audiomixer.py`) does exactly what
`MixerVoice.play()` already does — reset the sample, re-prime the voice's
buffer — for each voice that is still playing. A stopped voice stays stopped.
Every committed parity fixture still matches its recorded hash, because none
of them reset a Mixer with voices playing.

**Not applied to the CircuitPython target.** `apply_cp_patches.sh` only adds
modules; the one CircuitPython source it rewrites is `audiocore.get_buffer`'s
return type, which the parity harness needs to compare like with like. Fixing
DSP inside the oracle would erase the divergence this file exists to record.
The consequence is real and worth stating: on CircuitPython, an effect chained
directly after a Mixer is still silent. Anything in `audioeffects` that ends in
a Mixer — `MultibandCompressor`, `Harmonizer`, `Octaver`, `StereoWidener`,
`DynamicEQ`, `PingPongDelay`, `Exciter` — can be the last node in a chain
there, but not the middle of one. (`ParametricEQ` was on that list until the
peaking-EQ fix below let it drop the Mixer entirely.)

## Peaking EQ computed `b2` with the wrong sign (effects-extension tier)

`audioif_biquad_configure_w0()` builds a peaking bell (mode 4) from the RBJ
cookbook. Upstream computes

```c
b0 = 1 + alpha * A; b1 = -2 * cos; b2 = 1 + alpha * A;
a0 = 1 + alpha / A; a1 = -2 * cos; a2 = 1 - alpha / A;
```

`b2` is `1 - alpha * A`. The sign is not cosmetic: it is what makes numerator
and denominator sum to the same value at DC and again at Nyquist, which is the
entire premise of a peaking filter — unity everywhere except the band around
`f0`. With the plus, the numerator picks up `2 * alpha * A` at DC that the
denominator does not, and since `1 - cos(W0)` is very small at low
frequencies, that term dominates. The DC gain becomes
`1 + alpha * A / (1 - cos(W0))`, so a +6 dB bell at 1 kHz with Q of 1 at
48 kHz arrives as roughly **+21 dB at DC**, and it worsens as `f0` drops. It is
not a bell with a blemish; it is a bass shelf with a bell buried in it.

Measured through the built extension after the fix, a +6 dB bell at 1200 Hz
(Q 1, 8 kHz) reads +6.00 dB at center and 0.00 dB at DC and Nyquist, and a
−6 dB cut reads −6.000 dB. Before it, DC read +21.42 dB.

This is upstream's bug, not a porting error: CircuitPython 10.2.1 carries the
identical lines at `shared-module/synthio/Biquad.c:157-159`. It survived
because `PEAKING_EQ` is the one mode a synthesis library rarely reaches for —
nothing in this repository, in `audioeffects`, or in micropython-vst3's
instruments or soundtrack used it. `audioeffects.ParametricEQ` worked around
it by synthesizing bells out of notch and band-pass sections instead — cuts
as notches blended to depth through the Filter's `mix`, boosts as band-passed
Splitter branches summed back over the dry signal through a Mixer, which is
why it was capped at three boosts (a Splitter has four taps). All of that is
now one Biquad per band in a single Filter cascade.

**Deviation**: fixed here, so `PEAKING_EQ` diverges from `bin/circuitpython`.
Nothing else moves — no existing fixture reached mode 4, which is also why
`tests/parity/biquad_component_probe.py` now walks all seven modes.

Still present in upstream `main` as of 2026-08-27, not just in the pinned
10.2.1, so this one is worth reporting rather than waiting out. Drafted:
`docs/upstream-reports/peaking-eq-sign.md`.

## A stereo `Filter` shared one biquad state between the channels (effects-extension tier)

A biquad is a recursion: each output sample is computed from the two input and
two output samples before it. Upstream's `audiofilters.Filter` allocates one
`biquad_filter_state` per cascade stage and runs it across the whole
*interleaved* buffer, so when it processes a left sample, the "two samples
before it" are the previous right sample and the previous left one.

Two consequences, both measured:

- **Every frequency lands an octave high.** The recursion advances twice per
  stereo frame, so the filter effectively runs at double the rate its
  coefficients were computed for. A bell asked for 1200 Hz peaked near
  2400 Hz. `audioeffects` compensated for this with a `SPECTRAL_SCALE = 0.5`
  factor applied to every frequency before handing it to a Biquad.
- **The channels are not independent.** With left fed 2400 Hz, the left output
  measured +3.63 dB when the right carried 300 Hz and +4.89 dB when the right
  carried 2400 Hz — the left channel's level moved 1.26 dB because of a change
  the right channel made. Identical input in both channels came out 3.3 dB
  apart at 3 kHz. No scale factor can correct this one.

**This one is a catch-up, and it is CLOSED (2026-09-09).** Upstream fixed it
after 10.2.1, the CP pin has since moved to 10.3.0, and this port now carries
upstream's exact shape: `audiofilters_assign_filter_chain(..., channel_count)`
in `src/audiofilters/__init__.c`, `states[j * channel_count + channel]`, and
10.3.0's own `synthio_biquad_filter_sample()` single-sample entry point. So this
entry describes no difference at all any more — it is kept because the failure
it records (a stereo `Filter` sharing one biquad state between the channels,
which sounds like a filter that works and measures as one channel moving the
other by 1.26 dB) is worth being able to recognise again. The fix here was
arrived at independently and landed on the same design, which was reassuring
about both.

**Change**: one state per stage *per channel*, indexed
`[stage * channels + channel]`, with the buffer deinterleaved per channel and
chunked in whole frames so the channels stay in lockstep across chunk
boundaries. `filter_states_len` still counts stages, so callers are unchanged.
Fixed in both implementations — the usermod (`src/audiofilters/Filter.c`) and
the CPython extension (`BiquadState.process_s16` in `src/cpython/_audioif.c`,
which gained a `channels` argument). The two agree byte-for-byte.

After the fix the same bell peaks at 1200 Hz where it was asked to, both
channels read identically for identical input, and the left channel's level is
unchanged by the right channel's content. Mono and stereo now agree exactly;
`SPECTRAL_SCALE` and `_core.filter_hz()` are gone from `audioeffects`.

**Not affected**: synthio's per-note filters. `synthio_synth_synthesize()`
filters a *mono* `tmp_buffer32` with a per-note state and only then expands to
stereo (`src/synthio/__init__.c`), so every instrument renders identically
before and after. The blast radius was the `audiofilters.Filter` sites in
`audioeffects`.

Stereo `Filter` had no fixture anywhere in this repository before this change —
its only two uses in the suite pass `channel_count=1` — which is how both of
these survived. `tests/parity/biquad_component_probe.py` covers mono and stereo
across all seven modes, and its golden is captured from this port rather than
from CircuitPython, because of the two deviations above.

### What the two fixes changed in `audioeffects`

`SPECTRAL_SCALE` and `_core.filter_hz()` are deleted along with their thirteen
call sites, so a frequency handed to any class in the library is now the
frequency it filters at. `_core.check_hz()` replaces them: halving everything
kept the library clear of Nyquist by accident, and a biquad configured above
Nyquist folds its coefficients and produces noise silently, so the library now
refuses instead. `GraphicEQ` drops ISO bands that a low configured rate puts
out of range — the 16 kHz band needs better than 32 kHz to exist.

`ParametricEQ` is one Biquad per band in one Filter cascade, bells and shelves
alike; it no longer builds a Splitter or a Mixer, no longer caps boosts at
three, and exposes the sections as `.biquads` for parameter binding. An EQ with
every band flat returns its source untouched rather than a chain of unity
sections. `GraphicEQ` inherits all of it. `DynamicEQ` keeps its topology but is
no longer an approximation: RBJ's notch and 0 dB-peak band-pass share a
denominator and their numerators sum to it, so with the compressor idle the
split reconstructs the input — measured flat to 0.03 dB from 100 Hz to 8 kHz,
where before the shared biquad state leaked each channel's band into the
other's notch.

Measured after the sweep at 48 kHz: a +6 dB / Q 2 bell at 1 kHz reads +5.98 dB
at 1 kHz and ±0.1 dB two octaves out either way; a −9 dB / Q 1.4 cut at 500 Hz
reads −9.05 dB; `LowPass` at 1 kHz reads −3.00 dB at cutoff and −12.33 dB an
octave above. `tests/test_cpython_effects_library.py` pins the bell placement,
the flat-EQ passthrough, and the Nyquist refusal.

## The biquads were Q15, so they could not go low — now `audiobiquad` only (audioif#77)

> **SCOPE CHANGED 2026-09-09 (Brad's call on audioif#77).** This deviation now
> applies to **`audiobiquad`, which is ours, and to nothing CircuitPython has.**
> `synthio.Biquad` — and so `audiofilters.Filter` and a `Note.filter` chain —
> runs `audioif_biquad_cp_*`, CircuitPython's own Q15 arithmetic, on all three
> targets, because `docs/correctness-standard.md` holds a node CircuitPython
> also has to CircuitPython's bytes.
>
> Why the scope had to be decided at all: it had been applied on **one target
> only**. The CPython twin ran the widened kernel and desktop MicroPython ran
> CircuitPython's Q15, so `audiofilters.Filter` rendered different bytes on two
> of our own targets — 11 LSB apart by the eighth sample of an 800 Hz low-pass,
> and still opening. Nothing caught it because each target was graded against
> its own stored digest; `biquad_component_probe.py` is now in `verify_dsp`'s
> cross-interpreter set, and a planted 1-LSB coefficient change fails it.
>
> **What that costs, measured rather than estimated.** Holding `synthio.Biquad`
> to CircuitPython's arithmetic brings back its low-frequency fixed points.
> Below roughly 100 Hz the filter stops being the filter it was asked for, and
> the mechanism is one column wide — these are the Q15 coefficients the code
> actually computes, through its own `fast_sincos` rather than an exact cosine:
>
> | corner | `b0` true | `b0` in Q15 |
> |---|---|---|
> | 1000 Hz | 3.93e-03 | 129 |
> | 200 Hz | 1.73e-04 | 6 |
> | 100 Hz | 4.51e-05 | **1** |
> | 63 Hz | 1.87e-05 | **1** |
> | 40 Hz | 7.96e-06 | **0** |
>
> At 40 Hz the input coefficient rounds to **zero**, so the section has no input
> gain at all. What remains is a feedback recursion with poles at radius 0.9963,
> which settles on an integer fixed point and holds it. At 63 and 100 Hz `b0` is
> a single LSB against true values of 0.61 and 1.48, so the gain is already
> wrong by 64% and 32% before the recursion is reached.
>
> **It does not go quiet, it latches, and ordinary material triggers it.** Where
> the report draft says a low-frequency low-pass goes "silent", that is too kind.
> Fed 512 frames and then silence, a 40 Hz `LOW_PASS` at Q 0.707 settles on a
> constant and stays there:
>
> | material | parks at |
> |---|---|
> | DC burst | 16143 |
> | kick, 60 Hz decaying sine | 16143 |
> | bass note, 55 Hz | 16143 |
> | square, 80 Hz | 16143 |
> | noise burst | 16143 |
> | asymmetric saw, 45 Hz | 0 |
>
> Five of six, and at 100 Hz all six park at 2631. Feed it fresh audio
> afterwards and the output stays constant — `min == max == 16143` — so the
> section is not merely inaccurate, it has stopped passing signal.
> `audiofilters.Filter` exposes no `clear()`, so nothing short of building a new
> node recovers it.
>
> Identical on all three targets, so it is a fact about CircuitPython's kernel
> and not about a binding.
>
> **Scope, stated plainly, because the numbers invite overstatement.** This is
> a corner *we* live in and most callers do not: at 200 Hz and above the
> coefficients are fine, and synthio's own note filters sit far above it. We are
> trying to be an exact replica and to build instruments a musician would use;
> the projects synthio was designed for ask neither of those things (Brad,
> 2026-09-09). It is severe for us, it has a fix on our side, and it is not a
> fire under anyone else's chair.
>
> `docs/upstream-reports/biquad-band-edges.md` carries the report. It is **held**
> along with the other two — nothing goes upstream until we have done a great
> deal more work across the org (Brad, 2026-09-09).

### Which filter to reach for

**Above a few hundred hertz, `audiofilters.Filter` is fine** — the difference
from `audiobiquad` there is one to a few LSB, and it is the node CircuitPython
has, so it is the one a ported sketch will already be using.

**Below about 100 Hz, use `audiobiquad`.** It reaches exact zero on every one of
the cases above, where `synthio.Biquad` settles on a constant it cannot be
talked out of. The same applies to `audiobiquad.AllPass` against
`audiofilters.Phaser`, whose feedback is clamped to `0.1..0.9` so a true null is
not reachable.

The README's `audiobiquad` section links here rather than repeating any of it.

> Two smaller entries are **not** in this deviation's scope and stay applied
> everywhere, because upstream has already merged them: PEAKING_EQ's `b2` sign
> (CircuitPython `main` 8fabdbbfb1) and `synthio_biquad_filter_reset()` clearing
> all four state words rather than two (8a3deace5c). Neither is in a release
> yet, so each is a named departure from the pinned 10.3.0 oracle that expires
> when CircuitPython cuts a release containing it.
>
> The rest of this section is the original write-up. Read it as the case for
> `audiobiquad`'s arithmetic and as the record of a failure worth recognising
> again — not as a description of what `synthio.Biquad` does today.


Found while looking for somewhere to put a tape head bump, recorded as a
limitation, and then fixed one phase later once the user approved a third
deviation from the oracle. The section keeps its original measurements as the
"before" column, because the failure was silent and worth being able to
recognise again.

### What was wrong

`audioif_biquad_configure_w0()` stored its five coefficients as Q15 integers
(`AUDIOIF_BIQUAD_SHIFT = 15`, `scale()` rounding `value * 32768` to an
`int32_t`), and `audioif_biquad_process()` accumulated the five products in
`int32_t`. Both came straight from CircuitPython's
`shared-module/synthio/Biquad.c`, and both are defensible on a
microcontroller. The cost is that low-frequency sections are unrepresentable:
as `W0` goes to zero a low-pass's `b0 = (1 - cos W0) / 2` goes with it, and at
100 Hz / 48 kHz it is `4.3e-5`, which is 1.4 in Q15 and rounds to 1. Meanwhile
`a1` approaches `-2` and its product with a full-scale sample approaches
`INT32_MAX` on its own, so the accumulator has nothing left for the other four
terms.

There was a **second, independent** cause, found while fixing the first and
approved with it. `fast_sincos()` fits one 5th-order polynomial to both sine
and cosine over `[0, pi/2]`, and it is wrong at both ends of the audio band
for two different reasons:

- Its cosine carries up to `2.3e-5` of absolute error over the quarter it is
  fitted to, and `5.4e-6` at the `W0` a 100 Hz corner uses. Every low-pass,
  notch and shelf coefficient is built from `1 - cos W0`, which at 100 Hz /
  48 kHz is `8.6e-5` -- so that error is 6 percent of the answer, 13 percent at
  50 Hz and 34 percent at 20 Hz. Widening the fixed-point format alone would
  have left a 1 dB error at 50 Hz.
- `pi/2` in `W0` is only 12 kHz at 48 kHz, and above that the fit is
  **extrapolating**. At 20 kHz its sine is off by 6.6 percent and `1 + cos W0`
  by 15 percent; at 22 kHz, by 31 and 128. This half had never been noticed at
  all: a `HIGH_PASS` at 22 kHz passed its entire stopband.

### What it measured, before and after

At 48 kHz, one biquad, Q 0.707 unless stated, against the double-precision
closed form:

| asked for | before | after | ideal |
|---|---|---|---|
| `LOW_PASS` 50 Hz, at cutoff | -- | −2.99 | −3.01 |
| `LOW_PASS` 100 Hz, at cutoff | **silence** | −3.02 | −3.01 |
| `LOW_PASS` 200 Hz, two octaves below | **+1.25** | −0.03 | −0.02 |
| `HIGH_PASS` 30 Hz, at cutoff | **+21.6** | −3.04 | −3.01 |
| `HIGH_PASS` 100 Hz, at cutoff | **+6.24** | −3.02 | −3.01 |
| `LOW_SHELF` 80 Hz / +1.5 dB, at 20 Hz | **+13.4** | +1.50 | +1.49 |
| `LOW_PASS` 20 kHz, at cutoff | −3.71 | −3.01 | −3.01 |
| `LOW_PASS` 22 kHz, at cutoff | **−7.29** | −3.01 | −3.01 |
| `HIGH_PASS` 22 kHz, at cutoff | **+0.06** | −3.01 | −3.01 |

Everything from 50 Hz to 22 kHz now lands within 0.03 dB of the closed form.
The old usable floor was about 300 Hz, and 400 Hz for anything under half a
decibel of error.

**That "before" column is this port's, not upstream's**, and the two do not
match: measured on a build of upstream `main`, a `LOW_PASS` at 100 Hz reads
-3.94 dB rather than silence and a `HIGH_PASS` at 30 Hz reads +9.03 rather
than +21.6. Same two causes, same conclusion, different arithmetic on the way
(this port had already moved to per-channel state and computes its
coefficients in `double`, where `mp_float_t` on a board is `float`). The
upstream numbers, and a repro that produces them, are in
`docs/upstream-reports/biquad-band-edges.md`. **Do not quote this table
upstream.**

### The fix

`src/shared/audioif_biquad.c`, three changes:

1. **Per-filter coefficient format instead of a fixed Q15.**
   `choose_shift()` takes the largest of the five normalised coefficients and
   gives them all as many fractional bits as that one has room for in an
   `int32_t`, capped at 30. A plain low-pass tops out near 2 (that is `a1`)
   and gets 29 bits; a 20 dB shelf reaches ~200 and gets 23. Fixing one
   format for every filter would mean giving them all the shelf's. The chosen
   shift travels with the coefficients, so `synthio_biquad_t` caches it
   alongside `a1..b2` -- they are meaningless apart.
2. **`int64_t` accumulator, and a feedback state with 12 fractional bits below
   the sample grid** (`AUDIOIF_BIQUAD_STATE_SHIFT`). The second half matters
   more than it looks: a biquad low down has both poles close to the unit
   circle, and `1/A(z)` -- the gain the loop applies to whatever error is fed
   back into it -- is 4000 at DC for a 100 Hz low-pass and 43000 for a 30 Hz
   high-pass. Rounding the feedback to whole samples, as upstream does, hands
   that gain half an LSB of error to amplify.
3. **`fast_sincos()` replaced by `sine_and_cosine()`**: reflect into
   `[0, pi/2]` (`sin(pi - t) = sin t`, `cos(pi - t) = -cos t`) and evaluate
   the two Taylor series properly, seven terms each. Worst error across
   20 Hz -- 24 kHz falls from `5.4e-6` to `6.3e-9`, and it stays accurate past
   Nyquist, so a frequency asked for above Nyquist is now merely wrong rather
   than absurd. Deliberately **not** libm: glibc, newlib and MicroPython's own
   `sin`/`cos` differ in the last place, and `verify_dsp`'s one-hash-covers-
   every-interpreter rule depends on this being the same function everywhere.

### What it cost

Measured by cross-compiling `audioif_biquad.c` at `-Os` and counting
`audioif_biquad_process()`:

| core | before | after | note |
|---|---|---|---|
| Cortex-M4 / M7 | 37 instructions | 76 | no library calls -- `SMULL`/`SMLAL` |
| Cortex-M0+ | 55 instructions | 154 | 5 `__aeabi_lmul` + 1 `__aeabi_lasr` |

So roughly 2x on anything with a long multiply, and closer to 4x on
Cortex-M0+, which has none -- one stereo biquad at 22050 Hz goes from about
5 percent of a 48 MHz M0+ to about 17. That is the trade upstream made when it
chose Q15, and it is a real one on an RP2040. It is accepted here rather than
made conditional: a filter that sounds different on a Pico than on an ESP32-S3
would be worse than either. If it ever needs clawing back, the products are
32x32 into 64 and could be hand-written for M0 rather than going through
`__aeabi_lmul`.

### Consequences inside `audioeffects`

- `GraphicEQ`'s bottom three ISO bands were wrong. Asked for +6 dB and
  measured at its own center, the 31.5 Hz band gave **+12.14 dB**, 63 Hz
  +6.96, 125 Hz +3.07. All ten bands now read **+6.01 dB or better**.
- `MultibandCompressor`'s default `low_hz=200.0` crossover mis-split, leaving
  a **+5.2 dB** bump below 100 Hz. That is gone -- and with the filters
  working it exposed a separate defect underneath, a **3.4 dB dip** at the
  crossover, because the low band cascaded two Butterworth low-passes against
  the mid band's single high-pass. Both sides are Linkwitz-Riley pairs now and
  the three bands recombine flat to 0.23 dB from 30 Hz to 8 kHz.

Worth noting on the timing: before the effects-extension tier,
`_core.filter_hz` halved every frequency on the way in, so *both* of those ran
an octave lower still -- `GraphicEQ`'s 63 Hz band was configured at 31.5 Hz,
and the multiband low band was a 100 Hz low-pass, i.e. silent. Removing that
workaround moved them up out of the worst of it by accident.

### What moved, and what did not

- `golden/biquad_component.json` re-captured. The probe gained `biquad_edge`,
  seven modes across four centers from 60 Hz to 3600 Hz at an 8 kHz rate, so
  both ends are pinned by a fixture for the first time. Every one of the 28
  mode/center pairs moved.
- `golden/synthtools_acceptance.json` re-captured, and **it is no longer a
  byte-for-byte CircuitPython match** -- the first fixture in the suite to
  lose that. Three checksums moved: lead +0.020 percent, bend +0.24, bass
  **+1.50**. The bass is the tell: it is a Q 6.0 low-pass sweeping downward
  from 300 Hz at 22050 Hz, which is precisely the case that could not be
  represented before. `verify_acceptance.py` now keeps the oracle's own
  answer beside the port's under `circuitpython_stdout` rather than
  discarding it.
- All four `golden/instruments_*.json` re-captured, all 93 instruments. Every
  one of them builds a `synthio.Biquad`, so every one moved -- but both sides
  of that comparison run through the same engine, so the property the fixture
  exists to prove is untouched: **186 comparisons, 0 failures** after
  re-capture.
- **Not** `verify_dsp`, `verify_effects` or `verify_streaming`: no filter in
  any of their probes.
- Stock CircuitPython does not get this. `apply_cp_patches.sh` only *adds*
  modules to a CP tree; `synthio` and `audiofilters` there are upstream's, so
  a CP board still cannot filter below a few hundred hertz. See the effects
  README, "A note on filters off a stock CircuitPython board".

## `Distortion` ignores `drive` in OVERDRIVE mode (upstream, worked around)

`shared-module/audiofilters/Distortion.c` never reads `drive` in the
`DISTORTION_MODE_OVERDRIVE` branch -- the curve is a fixed shape. CLIP and
WAVESHAPE both use it, and the `drive` docstring says it is "the amount of
distortion" without noting the exception, so the argument looks connected and
is not. Measured: `drive` 0.0 / 0.2 / 0.5 / 0.9 in OVERDRIVE render byte-
identical output, while the same four in WAVESHAPE differ.

Still present on `main` 2026-08-27. Drafted for upstream as "wire it up or
document it": `docs/upstream-reports/distortion-overdrive-drive.md`. It may
well be intentional, which is why the draft asks rather than patches.

**Not fixed in the C here either.** `audioeffects` works around it instead --
`drive.py`'s `_push()` maps a 0..1 drive knob onto `pre_gain`, which is the
only way into that curve, with the level put back so the historical default
stays bit-identical. See phase 8 in the plan, and the `drive.py` docstring.

## A biquad reset cleared half its state (fifth approved deviation)

Found while preparing the upstream drafts. `synthio_biquad_filter_reset()`
does

```c
memset(&st->x, 0, 4 * sizeof(int16_t));
```

and `biquad_filter_state` is `int32_t x[2], y[2]` -- sixteen bytes, of which
that clears eight. `x[0]` and `x[1]` go; **`y[0]` and `y[1]` keep the previous
output history**, which is the filter's feedback memory. The `int16_t` looks
like a leftover from a time when the state was 16-bit.

Measured through `audiofilters.Filter`: fill the filter with a 200 Hz tone at
30000, call `reset_buffer`, then feed pure silence, and the first block comes
back with a peak of **28072** -- a clean exponential decay of the audio that
was supposed to have been cleared, at -1.3 dBFS. Both callers mean a full
reset (`audiofilters_filter_reset_buffer`, and `synthio.Note` when a note's
filter is initialised).

The fix is one line (`memset(st, 0, sizeof(*st))`), and it was verified here
by applying it to this port's `audioif_biquad_reset()` and re-running that
measurement: 28072 -> 0.

**Fixed here** -- the fifth approved deviation, taken 2026-08-27 --
and reported upstream as `docs/upstream-reports/biquad-reset.md`.

**Nothing moved.** Every golden held without re-capture: `verify_biquad`,
`verify_effects`, `verify_streaming`, `verify_acceptance`, `verify_dsp` on all
three interpreters, and all 93 instrument comparisons. That is worth
recording, because `synthio_note_start()` resets a note's filter on every
press and the obvious worry was that a re-pressed voice inheriting the
previous note's tail was baked into the fixtures. It was not: nowhere in the
suite does a biquad get reset with a non-zero `y`. So this deviation changes
what happens in the one case upstream leaves undefined and nothing else.

**Stock CircuitPython does not get it**, same as the other four:
`apply_cp_patches.sh` only *adds* modules, and `synthio`/`audiofilters` on a
CP board are upstream's. `bin/circuitpython` therefore still exhibits the bug,
which is correct -- it is the oracle.

Note that `common_hal_audiofilters_filter_play()` does *not* call
`audiofilters_filter_reset_buffer()`; it resets the source only. So a plain
`filter.play(other)` carrying filter memory over is by design, and is not this
bug.

## `audioconvolve`: audioif's own, and the one thing the palette could not fake (phase 12)

Nothing in CircuitPython transforms anything, and neither did
micropython-vst3's engine. `audioconvolve.Convolver` applies an impulse
response by uniform-partitioned overlap-save FFT convolution:
`shared/audioif_convolve.c` over `shared/audioif_fft.c`, `float` throughout.

**Why it is not a preset over the existing reverb.** `audiofreeverb` is a
fixed network of delay lines. It sounds like a room, and with the right
settings it sounds like a plausible room, but it cannot sound like a
*particular* one. Convolving with a plate's recorded impulse *is* that plate,
and the same node is then a hall, a guitar cabinet, a spring tank or a
telephone depending only on which impulse it was handed. That is a different
kind of thing from a preset, and it is the last entry on the catalogue that
the rest of the library genuinely could not approximate.

### The transform

`shared/audioif_fft.c` is a radix-2 Cooley-Tukey with a bit-reversal pass,
wrapped in the usual real-input packing: an N-point real transform runs on an
N/2-point complex one, so it costs half of what a naive complex transform of
the same block would. There is no split-radix and no hand-unrolled first
stage, because the honest bottleneck in a convolver is the pointwise multiply
across the partitions, not the two transforms either side of it.

Two decisions worth recording:

- **`float`, not `double`.** A double transform doubles the memory of every
  stored partition, and memory is what decides whether an impulse fits on a
  board at all. float32 gives ~7 digits and the transform's error grows as
  sqrt(log2 N), so a 512-point transform of int16 audio lands ~1e-3 out of a
  full-scale 32768 -- five orders of magnitude below the samples it is made
  of. Measured against numpy's `rfft`: 1.1e-7 relative at N=512.
- **The twiddles come from a series, not libm.** Same rule as the biquad's
  (see "The biquads are Q15" above): a golden hash of one probe has to match
  on CPython, MicroPython and CircuitPython, and three libms agree to within
  an ulp and differ in the last place. That is what
  `shared/audioif_trig.c` is for.

### `shared/audioif_trig.c` — extracted, not changed

The deterministic sine and cosine used to be `static` inside
`audioif_biquad.c`. The FFT needs the same guarantee for the same reason, so
they moved to a file of their own. **The biquad's arithmetic is unchanged**:
`audioif_sincos_reflect()` is the old function operation for operation,
reflecting about pi/2 only, and it is deliberately *not* "fixed" to
full-circle reduction -- a frequency above Nyquist would then get a different
wrong answer, and several goldens are pinned to this one. `audioif_sincos()`
is the new full-circle entry point, used only by the twiddle tables.
`verify_biquad`, `verify_effects`, `verify_acceptance` and all four
`instruments_*.json` were unchanged by the extraction, which is the check
that says so.

### Design decisions in the convolver

- **One partition of latency, accepted.** A block cannot be transformed until
  it is complete, so the output trails the input by 256 frames (5.3 ms at
  48 kHz). Removing that means a non-uniform partitioning scheme -- a few
  direct taps, then small partitions, then large -- which is roughly triple
  the code for a saving that matters only when monitoring a live player. A
  convolver with **no impulse loaded is a bypass with no latency at all**:
  an impulse that has not arrived is a missing setting, not a null room, and
  a chain built before its impulse arrives must not drift against its
  neighbours.
- **`mix` follows `audiofreeverb`, not `audiodelays`.** 0..1 with the dry at
  unity until halfway, rather than `Echo`'s 0..2. This is a reverb; matching
  the other reverb matters more than matching the delays.
- **The synthesized room is normalized to unit energy, in two passes.** A
  tail of unit-amplitude noise convolved with anything is enormous -- 48000
  taps near full scale sum to tens of thousands of times the input -- so an
  unnormalized synthetic room is not a quiet room, it is a clipped one. The
  partitions are transformed as they are generated and there is nowhere to
  keep the taps, so the deterministic generator simply runs twice: once to
  measure the energy, once to write it scaled. The second pass costs only the
  noise, not the transforms.
- **The noise is xorshift32 and the exponentials are a series**, for the
  determinism reason again. A room that is not bit-identical between builds
  is not a room, it is three rooms.
- **`reset_buffer` drops the history and keeps the impulse.** One is audio in
  flight; the other is a setting, and reloading a room because playback
  restarted would be both wrong and expensive.

### What it costs, which is the whole story on a board

Each partition holds 257 complex floats, about 2 KB, and there is one
frequency-delay line per audio channel plus one stored impulse per impulse
channel. So:

| impulse | partitions | memory | arithmetic |
|---|---|---|---|
| 1024 taps (21 ms) — a cabinet | 4 | ~25 KB | ~3 MFLOPS |
| 4096 taps (85 ms) | 16 | ~100 KB | ~12 MFLOPS |
| 1 second, stereo | 188 | ~1.5 MB | ~150 MFLOPS |

A cabinet is comfortable on a microcontroller. A second of stereo reverb is a
desktop or a render, or a board with PSRAM and nothing else to do. Both are
in `audioeffects`: `drive.CabinetSim` and `reverb.ConvolutionReverb`, and
each says so in its docstring.

### `audioeffects.CabinetSim` builds a filter's impulse, not a bell's

Worth recording because the first cut got it wrong. A cabinet's response was
modelled as a sum of damped sinusoids -- box resonance, presence peak, top
roll-off -- with decay times chosen by ear from the description. That gives
resonances of Q 17 and a peak gain of **818** at the box frequency: a bell,
not a box. Rebuilt as the impulse response of the filter cascade the
description actually names (a high-pass under the resonance, two peaking
bells, and two cascaded low-passes for the 24 dB/octave a cone rolls off at),
run over a unit impulse in float.

And the normalizer has to be the *response*, not the tallest tap: the cascade
sits several dB above unity at its bump, and a cabinet that multiplies
everything by four is a cabinet that clips. `_peak_response` sweeps the
cascade's magnitude at 48 log-spaced frequencies and normalizes by the
largest. Measured, `4x12 Stack`: +0 dB at 100 Hz, -3.9 at 1 kHz, -10.9 at
5 kHz, -27.0 at 8 kHz, -45.7 at 12 kHz.

Verified by `tests/parity/convolve_probe.py` through `verify_dsp.py`, with no
oracle -- the golden is captured from the port. It is the most
float-dependent fixture in the suite: every output sample is a sum of
hundreds of float products routed through two transforms. All three
interpreters render it identically, synthesized rooms included.

## Press semantics: two CPython-target divergences fixed (2026-09-01)

Issues #8 and #9, found by the accuracy program's fixed-circuit drum
rebuilds. The CPython target's `Synthesizer.press` deviated from
`synthio_span_change_note` three ways: it evicted the oldest note when
full (upstream refuses the new press), it evicted a bystander and leaked
a slot when a *member* was re-pressed at the cap, and it re-initialized
the envelope from zero on re-press where upstream re-enters ATTACK from
the current level with the oscillator phase intact. All three now mirror
the oracle and the re-press render is byte-identical with a built
CircuitPython (`tests/test_cpython_press_semantics.py` holds the
behaviors). MicroPython was already correct.

## Extension: `Note.filter` accepts a serial Biquad cascade (2026-09-01)

Issue #11, a deliberate extension beyond the oracle (like `audioecho`):
`Note.filter` also accepts a tuple/list of up to four `Biquad`s applied
in series, each stage with its own state — steeper slopes than one
biquad's 12 dB/oct can give a noise voice. A single filter behaves
exactly as stock CircuitPython, which never sees the extension: code
that must run on stock CP passes one Biquad (the usual portability
posture). MicroPython and CPython render cascades byte-identically;
per-note cost is one biquad pass per stage, and the resident state is
four biquad states per note on MicroPython (`SYNTHIO_NOTE_MAX_FILTER_STAGES`).

## Re-pressing a finished note drops it entirely (2026-09-02)

**Deviation from the oracle** (the sixth), and reported upstream.

Pressing a note that still holds its channel slot takes the fast path in
`synthio_span_change_note`, which sets `ATTACK` but deliberately leaves
`level` alone — correct while the note is still sounding, so it swells
back up from where it is and keeps its oscillator phase. But a note whose
envelope has already run down to 0 is *finished, merely not yet
collected*, and upstream treats it the same way: `ATTACK` with the level
left at 0. The reaper at the top of the render then tests `level == 0`
before anything steps the envelope, frees the slot and `continue`s, and
the envelope-advance loop skips freed slots — so the note is deleted
without ever being stepped. **The press renders byte-identical to never
having pressed at all.**

It is a one-block race: a block earlier the level is still above 0 and
the note re-attacks normally; a block later the slot has already been
reclaimed and the press takes the init path and works. Measured window,
sweeping the re-press block and reading the level at press time (oracle
and, before this fix, our MicroPython port):

| re-press at | level at press | note audible after |
|---|---|---|
| block 2 | 0.289 | yes |
| **block 3** | **0.000** | **no — silently dropped** |
| block 4+ | 0.000 (slot already reclaimed) | yes |

Musically this is a missed drum hit: every fixed-circuit instrument
presses several notes per circuit (a bass drum is body + noise click),
and a note-off followed by a re-strike onto the same circuit lands in
that window often enough to be heard.

**The fix, in both targets:** a re-press of a note whose level is 0 is a
*new hit*, not a swell — re-initialise its envelope and reset its phase,
exactly as a press on an already-reclaimed slot does. Applying the same
rule to both targets also makes the *timing* of slot collection
unobservable, which is why it does not matter that the C implementation
frees a slot as soon as the level reaches 0 while the CPython
reimplementation keeps a note until it is released: a silent note
contributes nothing either way, and the only thing that could depend on
whether its slot had been reclaimed was which press branch ran later.

`level == 0` is being used as a proxy for "finished", and during ATTACK it is
not one — the reaper's own comment says "note is truly finished", and a
note re-pressed one call earlier is not.

**Not closed by this:** the two targets are still not bit-identical on
full instrument sequences. That is a series of smaller lifecycle
differences between the C implementation and the Python reimplementation,
tracked separately; this entry covers only the dropped-note defect.


## Ring modulation was missing from the CPython target (2026-09-02)

Not a deviation — a **gap in our port**, now closed. `Note` accepted and
stored `ring_frequency`, `ring_bend`, `ring_waveform` and its loop bounds,
and the render loop never read them: a ringed note rendered byte-identically
to an unringed one, while the MicroPython usermod and the CircuitPython
oracle both applied the ring and agreed with each other exactly.

The stage is now ported from the usermod's own `synth_note_into_buffer()`,
including the parts that are easy to lose: the accumulator advances *before*
the sample is read; the product is narrowed to `int16` before it returns to
the `int32` voice buffer; and there are two separate guards, the first
bounding the rate against the ring table and the second — which reads as a
copy-paste at first glance but is not — bounding it against the *main*
waveform's limit and skipping the ring entirely rather than clamping. The
per-note ring accumulator is deliberately never reset on a press, matching
the usermod, which resets `accum` there but not `ring_accum`.

Verified bit-identical on all three runtimes. Eight instruments use ring
modulation (`cs80`, `cp70`, `b3`, `dx7`, `ms20`, `odyssey`, `rhodes`,
`wurlitzer`), so desktop renders of those were previously missing a feature
the boards had.

## Envelope reassignment was not live on the CPython target (2026-09-02)

**Not an upstream issue.** MicroPython, CircuitPython and the built oracle are
all correct; this was a divergence in this repository's CPython
reimplementation, now fixed to match them.

The C builds fetch a note's envelope on **every render block** —
`synthio_synth_get_note_envelope(synth, note_obj)` is called from the render
loop itself (`src/synthio/__init__.c:295`), and `synthio_envelope_state_t`
carries only the running state (level, substep, phase), never the parameters.
So assigning `note.envelope` is live: the next block steps the new envelope.

The CPython target built the definition **once, at press time**, and stored it
inside `note._envelope_state` (`_start_note`). A re-press took the reattack
path, which sets the phase back to ATTACK but never rebuilt the definition — so
the note kept stepping the envelope it was pressed with, no matter what had
been assigned since.

Invisible to a one-shot render. It only appears when **one circuit is shared by
two voices with different decays**, which is the idiom several `audioinstruments`
kits use for closed/open hats and for clap/maracas. Measured on a shared
claves+cowbell circuit:

| | alone | overlapped, before the fix | after |
|---|---|---|---|
| claves | 53 ms | 207 ms — inherited the cowbell's tail | 53 ms |
| cowbell | 203 ms | 55 ms — cut to the claves' tail | 203 ms |

The same code under the parent workspace's built `micropython` was already correct (57.3 / 204.3),
which is what identified the target rather than the instrument as the fault.

**Fix.** `_audioif.EnvelopeState` gains `set_definition(...)`, which replaces the
envelope parameters while leaving level, substep and phase untouched.
`Synthesizer._refresh_envelope` calls it from the render loop when
`note.envelope` is no longer the object the definition was built from —
`Envelope` is an immutable namedtuple, so identity is a sound and cheap guard,
and an unchanged envelope costs one comparison per note per block.

Verified on all three runtimes with the same script: a note pressed SHORT then
re-pressed LONG now decays over 405.3 ms on CPython, MicroPython and the
CircuitPython oracle alike; LONG then SHORT gives 58.6 ms on all three. Before
the fix CPython returned exactly the opposite pair. Regression coverage:
`tests/test_cpython_envelope_reassignment.py`.

## A released note held its channel for its whole tail (2026-09-02)

**Not an upstream issue.** The oracle is correct; this was a divergence in the
CPython target, now fixed to match it.

At capacity the oracle does **not** simply refuse. `synthio_span_change_note`
calls `find_channel_with_note` with `SYNTHIO_SILENCE`
(`src/synthio/__init__.c:361`), which scans the channels where the note is
*not playing* — released ones, still sounding out their tails — and takes the
**quietest**. Only when every channel is genuinely held does it return -1 and
refuse the press.

The CPython target dropped a note from `_notes` only once its envelope reached
zero (`_render`), while `press` refused at `len(self._notes) >=
max_polyphony`. So a released note occupied a slot for its entire release
tail, and a released note was indistinguishable from a held one as far as
admission was concerned.

Measured before the fix:

| | native engine | CPython target |
|---|---|---|
| blocks before a released note's channel returns | 1 | **758** |
| fresh press with all 14 notes released | accepted | **refused** |

And in use rather than in a probe: driving `rhodes` through the parity
sequence at capacity, **117 of its 122 refusals happened with no key held at
all**. The overwhelming majority of dropped notes were not a polyphony limit
being reached — they were the engine clogged by its own decaying voices. That
sequence never holds more than a four-note chord.

**Fix.** When no channel is free, `press` now takes the quietest *released*
note's slot, exactly as the oracle does, and still refuses when every channel
is held. Verified on all three runtimes with one script: all released → press
taken, all held → press refused and every held note intact, identical on the
CPython target, and the parent workspace's built `micropython` and `circuitpython`.

Regression coverage: `tests/test_cpython_released_reclaim.py`.

This was most of the difficulty behind audiocomponents#18, where the pianos
appeared to run out of voices far below the engine's ceiling.

## Four double literals narrowed under a 32-bit `mp_float_t` (2026-09-06)

`audioif`'s CI builds the unix port twice: once at the port's default
double `mp_float_t`, and once with `-DMICROPY_FLOAT_IMPL=MICROPY_FLOAT_IMPL_FLOAT`
so that a 32-bit `mp_float_t` — what every ESP32/RP2 board actually runs —
gets compiled at all. The unix port enables `-Werror` with
`-Wdouble-promotion` and `-Wfloat-conversion`, which MCU ports do not, and
under single precision four ported sites tripped them. The float cell used
to downgrade both classes to warnings; those downgrades are now gone and
the cell runs at full `-Werror`.

The four sites, all inherited from upstream CircuitPython's source text
(only the third is audioif's own code):

| site | was | now |
|---|---|---|
| `src/synthio/__init__.c:65` | `arg / 12. - 3` | `arg / MICROPY_FLOAT_CONST(12.) - 3` |
| `src/synthio/Synthesizer.c:169` | `level / 32767.` | `level / MICROPY_FLOAT_CONST(32767.)` |
| `src/audiodelays/Chorus.c:210` | `MAX(slot_get(...), 1.0)` | `MAX(slot_get(...), MICROPY_FLOAT_CONST(1.0))` |
| `src/audiodelays/MultiTapDelay.c:156,248,251` | implicit `double` → `mp_float_t` | explicit `(mp_float_t)` cast |

`MICROPY_FLOAT_CONST(x)` expands to bare `x` under
`MICROPY_FLOAT_IMPL_DOUBLE` (`py/mpconfig.h:967`) and `(mp_float_t)` is
`(double)` there, so **on desktop these six edits preprocess to the source
text that was already being compiled.** Shown, not assumed: every one of
the 115 objects the usermod builds — the four changed files included — is
byte-identical in disassembly, relocations and section contents before and
after (`objdump -w -d -r -s`, comparator proved able to fail by planting
`12.` → `12.5`, which reported exactly one moved object). No parity golden
can move, and none did.

**On a single-precision board, `midi_to_hz` does move.**
`common_hal_synthio_midi_to_hz_float` used to divide in double and narrow
the result; it now divides in float. Measured on two unix binaries built
either side of the edit: of 12801 fractional MIDI values, 5082 (39.7%)
differ, by at most 7 ulps of `float` — **0.0009 cents**, about a millionth
of a semitone. 54 of the 128 integer MIDI notes are among them (A440 is
not). Nothing in this workspace has ever captured a single-precision
render as a golden, so nothing goes red; the change is recorded here
because it is a real, if inaudible, difference in board output.

The other three sites are bit-identical in single precision too, checked
the same way: `1.0` is exact, and an explicit cast is only the implicit
conversion made visible. `Chorus`, `MultiTapDelay` and
`Synthesizer.note_info` all render identical bytes across the two float
binaries.

`MultiTapDelay`'s `tap_levels[]` stays `double` deliberately
(`src/shared/audioif_multitap.h:13`, and the rationale at
`MultiTapDelay.h:28-33`): the levels are converted once at set time so the
audio callback never marshals. Retyping it would change board DSP inside a
render loop, which is a different decision from silencing a warning.

## The voice ceiling is 64, where CircuitPython builds 14 (2026-09-03, recorded 2026-09-06)

CircuitPython sizes a `Synthesizer` at `CIRCUITPY_SYNTHIO_MAX_CHANNELS`,
and the oracle this port is measured against is built at **14** (the
`CIRCUITPY_SYNTHIO_MAX_CHANNELS` entry above). This port ships **64** at
all five sites that carry the number -- `src/synthio/__init__.h`,
`micropython.mk`, `micropython.cmake`, `src/cpython/synthio.py` and
`src/cpython/_audioif.c` -- moved together in `8f8b10d` by Brad, on #31's
evidence. 14 was the drum kits' number (cr78 holds exactly 14 permanent
Notes) and never the melodic library's, whose instruments press several
Notes per key; over the parity sequence 4743 of 7335 presses got a channel
only by evicting a still-decaying note, and a seven-note b3 chord measured
4.8 dB quieter at 14 than at 36. 64 covers a ten-finger chord on every
instrument. It is a choice, not a convergence point: steals keep falling
until roughly N=196.

**What it changes in the sound.** Two things, and the audible one is not
the limiter. First, a `Synthesizer` at 14 refuses or steals the Notes it
has no channel for. The melodic instruments press several Notes per key, so
a four-key chord on farfisa (5 Notes per key, 20 wanted) admits
`[5, 5, 4, 0]` channels at 14 -- the fourth key is silent -- and
`[5, 5, 5, 5]` at 64; vox_continental (4 per key) loses the top key's upper
partials at 14; minimoog (3 per key) fits its chord under 14 and only
differs where a new patch's chord lands on the previous patch's ringing
tail and the 14-channel engine steals decaying voices. That is what
`8f8b10d` was raised for, and it is what a listener hears: chord tones
present at 64 that are missing at 14. Second, and only for material that
also crosses the mix-down limiter's ±28000 knee, `SYNTHIO_MIX_DOWN_SCALE`
is a function of the ceiling -- 623 at 14, 129 at 64 -- so peaks above the
knee are squashed harder at 64. Measured by `tests/parity/verify_mixdown_knee.py`
(`de4f9db`), full-scale square voices on one Synthesizer, peak sample per
block:

| voices | peak at 14 (oracle) | peak at 64 (this port) |
|---|---|---|
| 1 | 16383 | 16383 |
| 2 | 28046 | 28010 |
| 3 | 28202 | 28042 |
| 6 | 28669 | 28139 |
| 10 | 29292 | 28268 |

A third of a decibel at ten full-scale voices. On real instrument material
the knee alone is inaudible: a minimoog chord held two seconds, both
renders peaking just over 28000, differs in 176 of 290304 samples by at
most 59 counts (measured 2026-09-06, renders under
`audiocomponents/.reference-captures/ceiling-ab/`). Below 14 requested
Notes and below the knee, every sample is byte-identical to CircuitPython's.
On audiocomponents' instruments gate, three of 53 instruments moved at the
gate's material -- farfisa, minimoog, vox_continental -- and at 64 the
CPython target and `cmods/bin/micropython` land on the same bytes for all
three (audiocomponents#24, audioif#27). Nine other melodic instruments press
more than four Notes per key and lose chord tones at 14 the same way; nobody
has yet counted how many change on the gate's material.

**How it is gated.** The four original parity gates are byte-identical at
14, 36, 56 and 64 because none of their material crosses the knee; they
cannot see a ceiling change and are not claimed to. `verify_mixdown_knee`
enforces this port's above-knee answer against this port and records the
oracle's beside it in the same golden; `tests/test_voice_ceiling_consistency.py`
guards a half-applied change across the five sites. The oracle stays at 14
and is never rebuilt. Material that crosses the knee is non-parity by
construction; everything under it still byte-matches.

**Still open on #31.** The prebuilt Windows interpreter, the wasm pair and
the micropython-vst3 sidecar carry 14 until rebuilt; the melodic instruments
that stack notes are a listen Brad reserved for himself when he made the
raise; audiocomponents' CPython leg follows the next audioif release, at
which point its three moved digests are re-captured at 64 with this entry
as the reason, and not before.

## `audiobiquad`: a biquad and an all-pass whose tails reach zero (effects program, Phase 1)

Additive, in a new module of audioif's own, and neither ported kernel is
touched. Both `dynamics_probe.py` and `route_probe.py` still hash to
`vstaudio_dsp.c` compiled unmodified, and the diff of
`tests/parity/golden/dsp_nodes.json` for this change adds one digest and
leaves the other seven byte-identical, which is the check that says the
addition is additive (audioif commit `48c3575` is the precedent).

**What was unreachable.** Every EQ, filter and phaser class in
[audiocomponents](https://github.com/PyDevices/audiocomponents) is held to
one invariant the effects program calls Tier 1 — *silence in, silence out; a
decaying tail reaches exact zero* — and on the ported nodes it is not a
matter of tuning:

- `shared/audioif_biquad.c` keeps its output memory in Q12 sample units
  (`AUDIOIF_BIQUAD_STATE_SHIFT`, `audioif_biquad.h:14`) and rounds to nearest
  at `:176-178` with no dither and no leak term, so the recursion has fixed
  points: states that reproduce themselves exactly. Driven with a 256-frame
  DC burst and then 3000 blocks of silence at 48 kHz, a `LOW_PASS` at 100 Hz
  settles on ±1 LSB and one at 40 Hz on ±4 LSB, which are the numbers
  audioif#23 reports. The reproduction is in
  `tests/test_cpython_audiobiquad.py` rather than cited, and it is careful
  about what it claims: **not every trajectory lands on a fixed point.** The
  same settings driven through the `audiofilters.Filter` wrapper with a
  different stimulus settled cleanly, which is why the defect went unnoticed
  for as long as it did.
- `shared/audioif_phaser.c` has its own version of the same defect in its own
  arithmetic. Its all-pass memory is `int16_t` in plain sample units
  (`:28-38`), so it settles on a non-zero word and holds it; fixing the
  biquad would not move it. Filed separately as audioif#36, and it is why
  this is one module carrying two kernels rather than a patch to one.
- `audiofilters/Phaser.c:211` clamps `feedback` to `0.1..0.9` before the
  value reaches the kernel, so a phaser built on the ported node can never
  be asked for the feedback-free topology every script phaser uses, and 0.0
  and 0.1 render byte-identically.

**What the module does instead.** `audiobiquad.Biquad` and
`audiobiquad.AllPass` over `shared/audioif_filter_f32.c`, `float` state and
coefficients throughout — the working precision `audioif_dynamics.c` and
`audioif_feedback_delay.c` already use. A `float` recursion decays
geometrically and so never actually *arrives*, so any state word below
`AUDIOIF_FILTER_F32_FLUSH` (1e-20) is written as exact zero. That threshold
sits far above the float32 denormal floor and far below one LSB of a 16-bit
sample, so it can only catch a tail already past −400 dB.

**A new module rather than arguments on `audiofilters`**, for exactly the
reason `audioecho` is a new module rather than arguments on
`audiodelays.Echo`: an argument added to this port's copy of a CircuitPython
module would not exist on a stock board, so a class written against it would
silently be a different effect there. This installs whole or is absent and
says so on import.

### The measurements

At 8 kHz, a 500 Hz tone (an integer-exact 64-entry sine table, stepped by
four) through a four-stage cascade at `mix=0.5`, dry peak 28000:

| | best null | re dry |
|---|---|---|
| `audiobiquad.AllPass`, `feedback=0.0` | 18 | **−63.8 dB** |
| `audiofilters.Phaser`, `feedback=0.1` (its floor), frequency swept over 100 settings and its own best taken | 2341 | −21.5 dB |

42 dB, and the ported node was given every chance: its `frequency` is not
the notch frequency, so it was swept rather than handed one number. On this
node `feedback=0.0` and `feedback=0.1` are different settings (peaks 18 and
1274); on the ported one they are the same bytes.

Tail to exact zero, burst then silence, first all-zero block: `LOW_PASS`
100 Hz at block 1, 40 Hz q=8 at block 16, 31.25 Hz at 1, a +12 dB peaking
section at 62.5 Hz at 9; the four-stage cascade at feedback 0.95 at block 6
and at −0.95 at block 7; a twelve-stage cascade at 31.25 Hz at block 30.
Every biquad mode and every feedback value tested reaches zero and stays
there.

### Three smaller decisions worth recording

- **The all-pass coefficient is the true bilinear**, `c = (tan(pi f / fs) −
  1) / (tan(pi f / fs) + 1)`, so `frequency` really is where one stage's
  phase passes −90° — verified within 0.2° at 500, 1000 and 2000 Hz.
  `audioif_phaser.c:14` uses `(1 − f/nyquist)/(1 + f/nyquist)` and multiplies
  by the negative of it (`:32`), which is the small-angle form of the same
  expression; that lands the break at `(fs/pi)·atan(2f/fs)`, and it is why
  the ported node's class has to pre-warp in Python
  (`audiofilters/Phaser.c:210`).
- **The all-pass coefficient walks to each block's value across the block**
  rather than stepping to it at the boundary. A block-rate step is 187 Hz at
  48 kHz with 256-frame blocks, in the middle of the range a phaser sweeps
  through. The biquad's coefficients are *not* interpolated: interpolating
  between two high-Q sections can pass through an unstable pair, and the
  biquad's case for existing is the tail, not the sweep.
- **Two bounds are stability, not taste, and both are there to protect the
  tail.** `Q` is clamped to 0.05..60, because the pole radius of an RBJ
  section is `sqrt((1 − alpha)/(1 + alpha))` with `alpha = sin(W0)/(2Q)`, so
  an unbounded Q is a filter that rings for ever and the invariant this
  module exists for would be vacuous. The all-pass coefficient is clamped to
  ±0.9999 for the same reason one order down. `feedback` is bounded to
  ±0.99 — the point of the ask is that zero is reachable and negative is
  allowed, not that the loop may be unstable.

`sqrt()` rather than `audioif_biquad.c:26`'s Newton-step reciprocal
approximation for the shelf root: that trick buys speed on a part with no
divider and costs about 0.2% of `A`, and it is there because the result is
about to be quantized into 23 bits. Nothing here is quantized, and IEEE
`sqrt` is correctly rounded, so this is both more accurate and identical on
every target. The trig *is* shared — `audioif_sincos_reflect()`
(`audioif_trig.c:72`) — so at one W0 both kernels start from the same sine
and cosine and a comparison between them measures the arithmetic rather than
two libm builds.

Verified by `tests/parity/filter_f32_probe.py` through `verify_dsp.py`, with
no oracle: the golden is captured from the port, the same weaker claim
`audiomath`, `audioecho` and `audioconvolve` make. That probe prints two
invariants as integers beside its PCM — the block at which a tail reaches
zero, and the null depth at each feedback value — because a hash over PCM
alone would not say whether either still held. Its planted fault is the
saturating int16 write-back the ported kernels use: restore it and ten of
the eleven tail cases go to "never reached zero", the null collapses from 18
to 1140, and the gate fails.

## `audioroute` gains `MidSide` (2026-09-07)

Additive, and the default is the identity, so an `audioroute` built the way it
was *is* what it was -- `tests/parity/route_probe.py`'s and
`route_dry_probe.py`'s hashes are unchanged across this addition, which is the
check that says so. `Splitter` still hashes to `vstaudio_dsp.c` compiled
unmodified. The new class gets its own fixture (`midside_probe.py`) rather
than joining either of those, because both are held against that oracle and
may only use forms the original accepts, and the original has no `MidSide`
at all: `audioroute` came from micropython-vst3's engine, this class did not.
It is audioif's own, like `audiomath`, `audioecho` and `audioconvolve`, so
there is nothing older to diff against and this section records a new class
rather than a deviation.

**What it is.** A stereo pair taken apart into its mono sum and the difference
between its channels, the difference scaled by `width`, and the pair put back
together. `width=0` collapses to mono, `1` passes through, `2` doubles the
sides; `set(width=)` moves it mid-stream and out-of-range values clamp to
those rails rather than being honoured, the same treatment
`audioif_multiply_set_mix` gives a mix outside 0..1.

**Why the palette could not already do it.** `audiomixer.Mixer` has a
per-voice `pan`, which *places* a source between the speakers. It cannot reach
what is already between them: no combination of pans collapses a pair to mono
or pushes its sides out, and `Splitter`'s branches all carry the same stereo
pair, never a mid on one and a side on another.

**Why a drive node lives in a routing module.** The traits that asked for this
are drive traits, not stereo-width ones. A nonlinearity applied to a stereo
pair intermodulates the channels -- what comes back on the sides is
sum-and-difference products of both -- so a stereo overdrive, fuzz or
saturator that wants to keep its image drives the mid and leaves the side
alone. That is a routing decision (which signal reaches which channel), which
is the job `Splitter` already does one level up (which branch reaches which
chain), so it goes here rather than into a module of its own. A new module
would have been a third import for every class that already needs `Splitter`,
and one more thing `apply_cp_patches.sh` has to install whole.

**The width is Q14, not Q15.** Every other integer coefficient in this tree is
Q15, because every other one runs 0..1. This one runs 0..2. Q15 would put
`width = 2` at 65536, and `65536 * 65535` -- 65535 being the widest difference
two int16 samples can have -- is 4294901760, past `INT32_MAX`, on the hot
path, on parts where a 64-bit multiply is not free. Q14 caps the same product
at `32768 * 65535 = 2147450880`, which fits `int32` with 32767 to spare. That
bound is checked rather than argued: a standalone driver over
`src/shared/audioif_midside.c` forms the widest product the kernel can reach
and prints it beside `INT32_MAX`.

**The halving comes last, and that is what makes the identity exact.** Per
frame the kernel computes `sum = L+R`, `diff = L-R`, `side = (w*diff) >> 14`,
then `outL = clamp16((sum+side+1) >> 1)` and `outR = clamp16((sum-side+1) >> 1)`.
At `width = 1` (`w = 16384`) a Q14 multiply is a shift left by 14 and the
shift right by 14 returns `diff` bit for bit, negatives included -- so
`sum + side` is `2L` and `(2L+1) >> 1` is `L` for every int16 there is. The
output bytes are the input bytes, with no special case in the code and none in
the API. Halving the sum first would lose a bit before the side was ever
added, and a `MidSide` left at its default would quietly cost the chain an LSB
wherever a class leaves one in a signal path it is not currently using.

The `+1` rounds half up rather than toward minus infinity. Without it every
odd result loses half an LSB downward, which is a small DC offset on anything
asymmetric -- exactly what a drive downstream turns into an audible bias. It
goes on both halves, so the mono sum lands on `L+R` or one more, never split
between the channels; `midside_probe.py` holds that to 1 LSB per frame at
every width, below the clamp.

**Latency and state: none of either.** No line, no filter, no accumulator, so
a block boundary is not observable, `reset_buffer` has only the source cursor
to drop, and no option can add latency later without changing what the class
is. That is the trait a `FeedbackDelay` composition cannot hold: its delay
clamps at one frame (`audioif_feedback_delay.c:81-83`).

**What the probe asserts that a checksum cannot.** Two things, both printed so
the golden covers them: at `width=1` the output bytes are compared against the
source bytes rather than against a hash of themselves -- a checksum would go on
matching a golden captured from a node that had started costing an LSB -- and
the mono sum is held to 1 LSB per frame at every width. Three planted faults
were run, each caught by a different mechanism: halving before adding breaks
the identity comparison; scaling the mid instead of the side leaves the
identity intact and moves the mono sum by 30000; dropping the `+1` leaves both
assertions passing and moves the checksum.

## `audiomath` gains `SubOctave`, an analog octave divider (2026-09-07)

Additive: a new class in an audioif-own module, nothing existing touched.
`tests/parity/multiply_probe.py`'s hash is unchanged across it, and so are
`dynamics_probe.py`'s, `route_probe.py`'s and `feedback_delay_probe.py`'s --
the diff of `tests/parity/golden/dsp_nodes.json` in the commit that landed it
adds one digest and changes none, which is the check that says so.

**Why the palette could not do it.** Nothing here divides a *frequency*:

- **`audiodelays.PitchShift`** is granular. It resamples grains and
  crossfades them, so an octave down arrives with the smear and the comb that
  windowing costs, and it costs a grain buffer per instance. It is a pitch
  shifter, which is a different device from a divider and does not sound like
  one.
- **`audiospeed.SpeedChanger`** moves the whole stream in time. An octave
  down there is the take played at half speed, which is not something a
  player can perform through.
- **`audiomath.Multiply`** rings the signal against a *free-running* table.
  Multiplying by a square that is not locked to the input is a ring
  modulator, and its sum and difference tones are exactly what a divider must
  not produce.

**What the circuit does, and what this does.** A Boss OC-2 and every pedal
like it squares the input up with a comparator, halves that square with a
flip-flop, and multiplies the *original* signal by the halved square. The
square is +/-1, so that multiply is a negate: the output is the input with
every other cycle inverted, which has twice the period, the input's own
timbre, and no latency at all. A second flip-flop in series gives `order=2`,
two octaves down. `shared/audioif_suboctave.c`, int16 in and out with Q15
coefficients and int32 intermediates -- `audioif_multiply.c`'s convention, no
float on the pull path. Two multiplies and a clamp per sample, one fewer than
`Multiply`'s three, plus three compares and a counter.

`SubOctave(source, order=1, mix=1.0, threshold=0.01, hold_ms=1.0,
sample_rate=48000, channel_count=2)`, with `set()` taking the same four
options mid-stream and `clear()` restarting the count. `mix` follows
`Multiply`'s 0..1 convention, not `Echo`'s 0..2: at 0.5 a signal and its own
inverse cancel exactly, which the parity fixture renders as a block of
digital silence.

Four things worth recording:

- **The defaults live in the C, once.** `audioif_suboctave_config_init()` is
  the only place `order` 1, `mix` 1.0, `threshold` 0.01 and `hold_ms` 1.0 are
  written down; every binding passes "not asked for" rather than a copy of
  the number, so the four targets cannot drift apart. Written the obvious way
  first -- with the values repeated in each binding's signature -- and caught
  by a planted fault: moving the C default did not move the golden, because
  no target ever reached it.
- **One divider drives every channel**, clocked by the mean of the frame.
  Two dividers, one per channel, would count differently the moment the
  channels differed, and the pair would then be an octave down in opposite
  polarities -- which cancels in mono.
- **`threshold` and `hold_ms` are the two ways of not counting a harmonic**,
  and both are the class's business, not the node's. A waveform with a strong
  second harmonic crosses zero more than twice per period, and a divider that
  counts the extra crossings drops an octave too far. Measured on a 110 Hz
  triangle carrying a second harmonic at 83% of its amplitude, rendered
  through `order=1, mix=1.0` and read as the magnitude at 55 Hz (right) and
  110 Hz (wrong):

  | `hold_ms` | `threshold` | 55 Hz | 110 Hz |
  |---|---|---|---|
  | 0.0 | 0.01 | 112 | **3816** |
  | 1.0 (default) | 0.01 | 112 | **3816** |
  | 6.0 | 0.01 | **6111** | 65 |
  | 0.0 | 0.25 | **5719** | 57 |

  A lockout longer than the harmonic's period fixes it; so does a threshold
  above the harmonic's own excursion. The **default of 1.0 ms does not**, and
  is not meant to: 1 ms is a period of 1 kHz, so the default tracks every
  fundamental a guitar or bass can produce and rejects only re-triggers
  closer together than that. A class built for one note range sets `hold_ms`
  from the lowest note it expects, or filters the input the way the pedal
  does. **The Phase 0 sketch proposed 8.0 ms**, which rejects the harmonic on
  a low note but silently divides by four above 125 Hz -- a wrong octave with
  no error, which is worse than a divider that counts what it was given.

- **`reset_buffer` restarts the count.** The divider holds no audio, so
  unlike a delay's reset this drops nothing anybody can hear; what it stops
  is a restarted chain beginning on the inverted half of the count.

Verified by `tests/parity/suboctave_probe.py` through `verify_dsp.py`, with
no oracle -- the golden is captured from the port under CPython and what it
proves is cross-interpreter agreement, not fidelity to something older. The
fixtures are integer-only for a reason a triangle does not have elsewhere in
the suite: a divider is decided by *where* the signal crosses a threshold, so
a fixture that moved by one LSB because two interpreters rounded `sin()`
differently would move an edge and change every sample after it.

**The probe was shown to fail before it was believed.** Twelve faults planted
one at a time in `audioif_suboctave.c`, each rebuilt and run: the second
flip-flop clocked on the falling edge; the lockout ignored; each of the three
defaults moved by one step (threshold 328 -> 329, hold 1.0 -> 0.9 ms and
1.0 -> 1.1 ms); the hysteresis made one-sided; the divider clocked by the left
channel alone; `order` ignored; the positive clamp rail moved; the Q15 rounding
term dropped; `reset` starting inverted; and the square never inverting. All
twelve moved the digest. Two fixtures exist only because a fault got through
the first version of the probe and did not: the two pulse trains at 45 and 50
frames, which are the only cases that pin the default lockout from both
sides, and the rails square, which was written with the channels in
opposition so that the frame mean was -1 and the comparator never fired at
all. The one thing the probe does **not** pin is the negative clamp rail,
which is unreachable: with `q = +/-s` the blend cannot fall below -32768, so
that branch is defensive symmetry with `audioif_multiply.c` and nothing
exercises it.

## `audioshaper`: audioif's own, and the two things a fixed curve cannot be (2026-09-07)

A new module, not a port and not from `vstaudio` either. It exists because
the palette's only steerable nonlinearity is `audiofilters.Distortion`, and
two facts about that node put every drive circuit out of reach.

**The curve is not yours.** Its whole argument list is `drive`, `pre_gain`,
`post_gain`, `mode`, `soft_clip` and `mix` (`audioif_distortion.c:7-9`), and
`mode` selects one of four fixed shapes written for a game engine
(`:15-34`). A diode pair in an op-amp's feedback loop, diodes to ground, a
biased germanium transistor: each is a specific curve, and none of the four
is any of them. Worse, two of the four are odd-symmetric by construction --
CLIP is `pow(fabs(value), drive)` with the sign restored (`:16-19`), and
WAVESHAPE is `(1+d)x / (1+d|x|)` (`:30-32`) -- so neither can produce an even
harmonic at all, and CLIP is homogeneous, so its harmonic profile is
identical at -20 dBFS and at -6 dBFS where a circuit's is not.

**It runs at the base rate.** A nonlinearity makes harmonics above Nyquist
and they fold straight back onto the signal. Nothing else in audioif
resamples either: `audiospeed.SpeedChanger` steps `phase >> SPEED_SHIFT` with
no interpolation and no anti-alias filter, and is itself a CircuitPython
port.

### What the node does instead

`audioshaper.Waveshaper` takes the curve as **int16 Q15 data** -- computed
once on CPython, where a dossier can show the circuit maths that produced it,
and never rebuilt on the target, whose float is single-precision where the
desktop's is double, so a table built on a board would be a different table
-- and applies it at **2x, 4x or 8x** the sample rate between a matched pair
of two-path polyphase all-pass half-bands. `pre_gain` is the drive knob (gain
into one normalised curve, never a curve rebuilt per knob move), `bias` moves
the operating point, `post_gain` and a 0..1 `mix` follow, the last being
`audiofilters.Distortion`'s convention (`audioif_distortion.c:47`) rather
than `audiodelays.Echo`'s 0..2, because this is the node the drive family is
leaving.

**A new module rather than arguments on `Distortion`**, for the reason
`audioecho` is not `audiodelays`: an argument added to audioif's copy of a
CircuitPython module would not exist on a stock board, so an `Overdrive`
written against it would silently be a different effect there. This installs
whole, through `apply_cp_patches.sh`, or is absent and says so on import.

### The half-band coefficients are measured, not quoted

`tools/design_halfband.py` is where they come from: a minimax search over
four coefficients for the stopband peak of
`H(w) = (A0(e^{j2w}) + e^{-jw} A1(e^{j2w})) / 2` from `0.5833*pi`. That
stopband edge is what the *last* decimation stage of a 48 kHz chain needs --
it runs at 96 kHz, folds everything above 24 kHz down, and has to stay clear
of 20 kHz -- so the transition band is 20..28 kHz. `--verify` is the check
that the numbers in the C are the numbers the search produced:

    passband ripple +0.00000 / -0.00000 dB (to 0.4167 pi)
    stopband peak   -64.57 dB (from 0.5833 pi)
    gain at pi/2    0.7071 (the half-band condition wants 0.7071)
    all poles at |z| = sqrt(a) < 1

Four coefficients rather than six, and that is a measurement rather than a
preference: with an identity curve loaded, the whole up/down chain's
non-harmonic energy sits 88-89 dB below the fundamental at every factor,
which is the int16 output's own quantisation floor. A six-coefficient design
(-68.8 dB in the same search) moves nothing this node can measure.

### What the oversampling buys

Measured on the built CPython extension with an exact-bin DFT -- 65536
points, 1379 and 5051 cycles in the window, so every harmonic and every alias
lands on its own bin and a rectangular window leaks nothing -- through a soft
+-0.6 knee at `post_gain` 0.5, non-harmonic energy against the fundamental:

| | 1010 Hz, pre_gain 8 | 3700 Hz, pre_gain 8 | 3700 Hz, pre_gain 2 |
|---|---|---|---|
| x1 | -36.2 dBc | -19.3 dBc | -31.9 dBc |
| x2 | -47.6 | -33.2 | -45.1 |
| x4 | -55.8 | -41.6 | -56.1 |
| x8 | -57.0 | -43.4 | -63.9 |

Two things that table says and a bare "it oversamples" would not. The probe
frequency is part of the measurement: at 1000 Hz exactly, every alias folds
back onto the harmonic grid and the same node reads a floor that is not
there. And the -60 dB bar the drive dossiers ask for is a function of curve,
drive *and* factor together, never of the factor alone -- at heavy drive a
near-square curve's own in-band aliasing sets the floor and a further
doubling does not move it. **The alias floor at each factor on the P4 and the
S3 is Phase 1's, measured on flashed firmware; this is the desktop figure the
board table is measured against.**

### Latency, which is not zero

Measured by output phase against input phase over 200 Hz..5 kHz at 48 kHz:
group delay 0 samples at x1, 2.2 (46 us) at x2, 3.3 (69 us) at x4, 3.9 (80
us) at x8, with the magnitude response flat to within 0.001 dB from 200 Hz to
18 kHz. It is small -- a linear-phase FIR half-band steep enough for this
transition would be several times that -- but it is not zero and it is not an
integer, so `audioshaper.GROUP_DELAY_SAMPLES` carries the table and a
component reporting `latency_samples` reports the entry for the factor it
built with.

### `hysteresis`: default off, and the half-width is a coercivity

Additive and off by default, so a `Waveshaper` built without it is a static
table sample for sample -- `waveshaper_probe.py` prints
`shp hysteresis-off identical` and `shp hysteresis-on changed`, so the golden
pins both halves of that claim and neither can be met by the option doing
nothing.

Above zero it puts a *play* (backlash) operator in front of the table: one
position per channel that follows the signal at a lag of one half-width, so
the rising and falling branches separate. This is the one thing a table
cannot do. A static curve's output depends only on its present input, so a
slow triangle in and out retraces its own path and encloses exactly zero
area, which is the disconfirmation condition of the Saturation dossier's TP3
met a priori, without a measurement.

**`hysteresis_width` is a fraction of full scale at the node's *input*, and
`config_finish` multiplies by `|pre_gain|` once, at configure time.** That
factor is the whole entry. Written the other way -- a half-width in the
operator's own units, the operator sitting after the drive gain -- twice the
drive covers half as much of the input's swing and the enclosed loop
*shrinks*: normalised area 0.163, 0.084, 0.045 over `pre_gain` 1, 2, 4,
measured before the fix. That is precisely the direction the two memory
elements the palette already has fail TP3 in, and the Phase 0 seed drove both
rather than arguing them: `audioecho.FeedbackDelay`'s normalised area is
*largest* with its nonlinearity switched off and falls when `loop_drive` is
engaged, and `audiodynamics.Dynamics`' shrinks with drive and swings two
orders of magnitude with probe frequency, because its loop is an envelope
artefact against the period rather than a quasi-static magnetisation loop.
Shipping a third node that fails the same way would have been no use to
anybody.

`hysteresis_bias` splits the half-width between the rising and falling
branches, so the loop may be asymmetric. There is no direction flag: the
comparison that fires already says which branch is which, so the flag the
Phase 0 sketch carried would have been a byte of state nothing reads.

### Two things the Phase 0 sketch asked for that are not here

- **`pre_gain` and `bias` are plain floats, not `BlockInput`s.** The sketch
  allowed them as streams because `Tremolo` B4 and the Fuzz Face's gating
  move the operating point per sample. B4 is struck: both of its refuters
  showed, and Arthur's ruling of 2026-09-07 accepted, that a per-sample bias
  is a second stream summed in front of a static shaper, which
  `audiomixer.Mixer` does sample by sample with a saturating signed add
  (`src/audiomixer/Mixer.c:216-230`). No surviving claimant needs a stream
  here, and keeping the kernel free of `mp_obj` is what lets one golden hash
  cover every interpreter -- the arithmetic all lives in `src/shared/`, so
  two interpreters disagreeing would itself be the finding.
- **There is no `pre_filter_hz`.** Vision section 6 says the input filter is
  the class's to compose, and `audiofilters.Filter` with a `synthio.Biquad`
  composes a better one than a one-pole inside this node would be. A node
  option that duplicates a palette node is what the "compose the existing
  palette first" rule refuses.

### How it is gated

`tests/parity/waveshaper_probe.py`, oracle `None` -- there is no ancestor to
hold it to, so what the golden pins is that every interpreter renders the
same bytes. Its curves are built out of integer arithmetic on purpose: a
table computed with `math.exp` would be a *different table* under
CircuitPython, whose floats are single-precision, and the probe would then be
measuring three libms rather than this node.

`tests/test_cpython_waveshaper.py` carries what the golden cannot: that
oversampling actually lowers the alias floor, that the hysteresis knob is
monotone and clears its control by 6 dB, and that a static table and a
zero-width operator both enclose exactly 0.0. Each check was shown to fail
before it was believed -- discarding the play operator's result reddens both
hysteresis tests, and replacing the half-bands with a zero-order hold and a
decimating drop reddens the alias-floor test.

## `audioladder`: the loop CircuitPython's filters cannot close (effects Phase 1)

`audiofilters.Filter` cascades RBJ biquads and does it very well: measured
against the analytic four-pole ladder response it tracks the resonant peak to
-0.05 dB at every feedback through 3.95. What it cannot do is not a matter of
degree. It is **linear**, and three of the ladder's fixed traits are things a
linear filter does not have at any setting:

- **It sustains.** At a feedback of 4 a ladder's loop gain reaches 1 at its
  own cutoff and a tone appears out of silence. A biquad cascade at any Q
  decays; fed silence it stays silent. The trait fails at its own definition.
- **What bounds that tone is a nonlinearity inside the loop.** Not a limiter
  after the filter, which would bound the level and leave the tone a sine:
  the saturator is in the feedback path, so it is what sets the amplitude,
  and its odd curve is where the growl comes from. A linear filter has no
  harmonic ladder to measure at all.
- **Its character follows the input level**, for the same reason. A linear
  filter's is identically level-independent, which is that trait's own stated
  disconfirmation.

Nothing else in the palette closes a loop of the right shape. An audioif
graph is a pull DAG in which no node accepts its own output, so a loop exists
only inside one C kernel, and no kernel held four one-poles round a feedback
path with a saturator in it. `audiodelays.Echo`'s loop is
`echo * decay + sample` into a delay line -- one tap, no filter, no saturator
(`audioif_echo.c:24`, `:31`). `audioecho.FeedbackDelay`'s loop has a low-pass,
a high-pass and a cubic clip in it, but behind a delay line clamped to at
least one frame: that is a comb, whose resonances are harmonics of 1/delay,
not one movable cutoff. And a loop closed in Python runs at block rate, about
187 Hz at 48 kHz, which is not a filter.

`audioladder.Ladder` is four topology-preserving one-pole stages round a
global feedback loop with the odd cubic saturator inside it -- the same curve
`audioif_feedback_delay.c` puts in its own loop, in the +-1 domain rather than
the int16 one. `shared/audioif_ladder.c`, `float` working precision to match
`audioif_dynamics.c` and `audioif_feedback_delay.c`. The options are
`cutoff_hz`, `resonance` (0..4.2), `drive` (a linear gain into the loop),
`poles` (1..4, which stage the output is tapped from), `passband_comp`,
`oversample` (1 or 2) and `mix` (a plain crossfade).

**A new module rather than arguments on `audiofilters.Filter`,
deliberately** -- the same reason `audioecho` is not arguments on
`audiodelays.Echo`. An argument added to audioif's copy of a CircuitPython
module would not exist on a stock board, so a `LadderFilter` written against
it would silently be a different effect there. A new module either installs
whole or is absent and says so on import.

Three details worth recording.

### The feedback has no delay in it, and that is the whole node

The obvious way to write this is to feed back the previous sample's fourth
stage. It does not work, and it fails at exactly the trait the node exists
for. Four bilinear one-poles reach -180 degrees at the cutoff with a gain of
1/4, so the loop sustains at a feedback of exactly 4, at exactly the cutoff,
and not before. One sample of delay adds its own phase, which moves the
180-degree point *below* the cutoff, where each stage is louder -- so the
loop sustains early, and at the wrong frequency. Measured on the first draft
of this file: 3.52 rather than 4 at a 1 kHz cutoff and 48 kHz, oscillating
six percent flat, and worse the higher the cutoff goes, since the delay's
phase is a fixed fraction of the sample rate while the filter's is not.

So the loop is solved instead of delayed, and the solve costs no division, no
library call and no branch on the signal. The loop equation is
`y + a*sat(y) = c`, with `a = resonance * g^4` and `c` what the chain would
put out with the feedback disconnected. Where `|y| >= 1` the saturator is
constant at 2/3 and the equation is linear, so that branch is exact and
closed form -- and the test for it is exact too. Inside, `sat(y) = y - y^3/3`
exactly, so it rearranges to `y = (c + a*y^3/3)/(1 + a)`, a map whose
derivative is `a*y^2/(1+a)` -- below 1 for every `a` and every `|y| <= 1`, so
it contracts from any seed. Four passes, seeded by extrapolating the last two
solved samples, with two constants computed in `set()` and three multiplies a
pass. The count is fixed rather than tested for convergence on purpose: a
convergence test branches on the signal, and two interpreters that took
different branches would render different bytes.

### `tan()` in `set()`, never in the loop

`g = tan(pi*fc/fs')/(1 + tan(pi*fc/fs'))` and everything derived from it are
recomputed whenever any option changes, which for a class with a macro on it
is once a block -- about 187 Hz at 48 kHz. The integrator state is kept
*unscaled* for that reason: the per-sample loop recomputes `(1-g)*z` rather
than storing it, which costs one multiply a stage and is what lets
`cutoff_hz` move mid-stream without the stored state silently meaning
something else afterwards.

`fs'` is the sample rate times `oversample`, so `oversample` moves the rate
the filter is measured against; `cutoff_hz` is therefore stored as asked for
and clamped where it is used, not on the way in.

### `mix` is a crossfade here, and 0..2 in `audioecho`

Deliberately different, and this is the place it is written down.
`audiodelays.Echo`'s convention -- dry at unity until 1, wet alone at 2 --
exists because a delay's wet signal is something *added* to a signal that is
still there, and six delay classes sit on `Echo` and `FeedbackDelay` where
`mix` has to mean one thing. A filter's wet signal is that same signal,
changed. "The dry plus all of the filtered" is not a setting any filter has.
So `mix` is 0..1 and crossfades, defaulting to 1, and `mix=0` is a wire
sample for sample -- the dry path takes the raw input, never the driven one.

### What was measured

Through the Python surface, `RawSample -> Ladder -> get_buffer` at 48 kHz, an
impulse then three seconds of silence:

| cutoff | k=3.0 | k=3.5 | k=3.9 | k=3.95 | k=4.0 | k=4.05 | k=4.2 |
|---|---|---|---|---|---|---|---|
| 200 Hz | silent | silent | decays | decays | sustains | sustains | sustains |
| 800 Hz | silent | silent | silent | silent | sustains | sustains | sustains |
| 3 kHz | silent | silent | silent | silent | sustains | sustains | sustains |

and the sustained tone measures 199.5, 800.1 and 3000.0 Hz against cutoffs of
200, 800 and 3000. At k=4.2 and an 800 Hz cutoff the peak moves 0.01 dB
across the last two seconds of the three and the tone is 0.07 % THD; its
second harmonic is 132 dB below the first and 69 dB below the third, which is
what an odd nonlinearity looks like. Driving a 200 Hz tone into the filter at
k=3.9, h3/h1 rises monotonically 45 dB across drive 0/+8/+16/+24 dB. THD
rises monotonically with input level, 0.010 % at -40 dBFS in to 0.302 % at 0.
Feeding `-x` returns exactly `-y` wherever the signal is off the int16 rails,
where the asymmetry is two's complement's (`+32767` against `-32768`) and is
the same in every other node here.

Verified by `tests/parity/ladder_probe.py` through `verify_dsp.py`, with no
oracle -- the golden is captured from the port, as `audiomath`, `audioecho`
and `audioconvolve` are. It is the most sensitive fixture in that file: the
loop is recursive, runs in `float`, *and* is solved, so a one-ulp
disagreement between two builds has the solver's seed to grow through as well
as the feedback path, and several of its cases sit where the loop sustains a
tone of its own and nothing damps a difference at all.

## `audioverb`: a reverberation tank whose network comes from Python (2026-09-07)

`audiofreeverb.Freeverb` exists upstream, and it is the only reverberator the
palette had. It is a *fixed* Schroeder/Moorer bank: eight combs and four
all-passes, with `roomsize`, `damp` and `mix` on top. Three things follow that
no argument on it could reach:

- **The network cannot be re-cut.** Its line lengths are `static const` tables
  in a CircuitPython-ported kernel -- `default_comb_sizes`
  (`audioif_freeverb.c:6`) and `default_allpass_sizes` (`:8`) -- and the
  exported entry point hands those two to the loop unconditionally
  (`:81`-`:82`), so nothing a binding can pass reaches them. A plate, a hall, a
  room and a chamber are not one topology at four settings; they are different
  line-length sets and different tap positions, and Freeverb's are compiled in.
- **There are no modulated taps.** Each comb and each all-pass walks its index
  by exactly one per sample and wraps (`:39`, `:49`); there is no oscillator in
  the file at all. A static plate rings with a picket fence of fixed modes, and
  breaking it means wobbling a line *inside* the loop by a fraction of a
  millisecond, which nothing outside the loop can do.
- **Nothing diffuses the input.** Freeverb's four all-passes sit on the *sum*
  of the comb bank (`:43`-`:51`), after the recirculation rather than before
  it, so what enters the combs is the raw signal and the early density is
  whatever the comb spacing happens to give.

**And the palette cannot compose one, which was measured rather than assumed.**
A tank's defining element is recirculation, and the pull graph has no cycles:
`d1.play(s); d2.play(d1); d1.play(d2)` constructs without complaint and the
first `audioecho.get_buffer(d1)` raises `RecursionError: maximum recursion
depth exceeded`. The only Python-side loop that exists at all quantises every
line to one 256-frame block -- 5.33 ms at 48 kHz, longer than most of a
plate's lines.

`audioverb.Tank` is Dattorro's plate network in `shared/audioif_tank.c`,
`float` working precision over `int16` lines to match
`audioif_feedback_delay.c`: predelay, a one-pole `bandwidth_hz`, an optional
`low_cut_hz` and cubic `drive`, four Schroeder all-passes, then two tank
halves that feed each other -- each a modulated all-pass, a delay, an in-loop
`damping_hz` one-pole, the `decay` multiply, a second all-pass and another
delay. The stereo comes back out of the tap table, then `width`, a tilt at
`tone_db` and `mix`. `delays` is twelve line lengths and `taps` is four values
per tap (channel, line, offset, gain); both default to Dattorro's published
table scaled from 29761 Hz, so a bare `Tank(sample_rate=...)` is his network
and not a transposed one.

**A new module rather than arguments on `Freeverb`, deliberately** -- the same
reason `audioecho` is not `audiodelays`. An argument added to audioif's copy of
a CircuitPython module would not exist on a stock board, so a `Plate` written
against it would silently be a different effect there. `audiofreeverb` is not
touched by any of this.

Four details worth recording:

- **Every line write is a magnitude truncation, not a rounding, and that is
  what makes the tail reach exact zero.** A recirculating `int16` network that
  rounds to nearest has limit cycles by construction: a line holding 1 with
  `decay` 0.5 computes 0.5, which rounds back to 1, and the tank hums at one
  LSB for as long as anything pulls it. Truncating toward zero makes
  `|Q(v)| <= |v|` unconditionally, so a loop under unity gain strictly loses
  magnitude every pass and lands on exact zero. It is the standard cure for
  fixed-point limit cycles and it costs nothing. **Proved by planting the
  fault:** with `tank_quantize` rounding instead, the probe's tail fixture
  sits at a block peak of 4 for 1500 blocks (48 s at 8 kHz) and never reaches
  zero; with the truncation restored it is exactly zero at block 81 (2.59 s,
  `decay` 0.5) and at block 348 (11.14 s, `decay` 0.9, against an RT60 of
  about 8.5 s). The *output* quantiser still rounds -- it is in no loop.
- **The cross between the halves reads a delay line, not the other half's
  output.** Each half's contribution is taken from its last delay line before
  anything in that frame writes to it, so it is the value that entered a whole
  line-length ago and neither half waits on the other. There is no algebraic
  loop and no one-sample fudge.
- **One `diffusion` knob drives four all-pass coefficients**, as ratios of
  Dattorro's own: input diffusion 1 at the knob, input diffusion 2 at
  0.8333 of it, decay diffusion 1 at 0.9333 and decay diffusion 2 at 0.6667.
  `diffusion=0.75` therefore reproduces his 0.75 / 0.625 / 0.70 / 0.50
  exactly, and the knob moves all four together the way a diffusion control is
  expected to.
- **`reset_buffer` really does drop everything**, unlike `audiodynamics`. A
  reverberation tail is entirely state: a chain restarted with the old tail
  still in the lines plays the previous take underneath the new one. And, like
  `audioecho.FeedbackDelay`, the lines only advance for frames that arrive --
  a starved chain gets silence and the tail stops with the source rather than
  ringing on. A class that wants the tail rung out feeds the tank silence for
  as long as its `tail_samples` says.

**What it costs.** Counted from the source, with everything switched on and
the default 14-tap table: about 62 multiplies per stereo frame -- input
fold-down 1, bandwidth 1, low cut 1, drive 6, four input diffusers 8, the two
tank halves 16, the modulation oscillator 2, the taps 14, width 3, the tilt 6,
the output mix 4. `audioif_freeverb.c` spends 30 `int16` multiplies per sample
per channel (`:28` the input scale, then `:36` twice and `:38` once in each
of eight combs, `:42` the sum scale, `:52` the output scale, `:53`-`:54` dry
and wet, plus the mix-down's `pair_scale` at `:56`), which is 60 per stereo
frame: the same order, in float rather than Q15. Its four all-passes cost no
multiply at all -- they are a shift and a subtract (`:47`-`:48`).

Measured on this desktop (x86-64, CPython, the two kernels called directly on
four seconds of stereo at 48 kHz, best of five), the tank is the *cheaper* of
the two -- 13.1 ms against Freeverb's 48.7 ms, 306x real time against 82x --
because Freeverb's saturating Q15 helper costs more on a part with a hardware
FPU than the tank's floats do. **That ratio is a desktop fact and does not
transfer**: the ESP32-S3 and ESP32-P4 figures are the effects roadmap's Phase
1 cost-table item, produced by the board cost runner on flashed firmware, and
nothing here estimates them from this.

Memory is the other half of the cost, and it is the part a board feels first.
The default table needs 36,280 `int16` (70.9 KB) of line at 48 kHz, plus the
predelay: 89.6 KB with the 200 ms default, 41.2 KB at 22.05 kHz. A shorter
`delays` table is how a lean character pays less.

Verified by `tests/parity/tank_probe.py` through `verify_dsp.py`, with no
oracle -- the golden is captured from the port, the same weaker-but-stricter
claim `audioecho` and `audioconvolve` make. It is the most recursive fixture
in the suite: ten lines feeding each other in `float`, the two halves crossed,
so a one-ulp disagreement between two builds is amplified for thousands of
frames rather than staying one ulp. The probe's own last two lines are not
checksums -- `wire-exact` says `mix=0` came out bit-identical to what went in
while the whole network ran, and `tail` is the block at which the wet output
reaches exact zero, which is the fixture a build with a rounding quantiser
would fail while still hashing everything else plausibly.

## `audiomodal`: a bank of resonators, which a chain of biquads cannot be

Nothing in CircuitPython does this, and neither did micropython-vst3's engine.
`audiomodal.Bank` rings a signal through N two-pole resonators in parallel,
summed in `float` and quantised once: `shared/audioif_modal.c`, with the
pole angle from `shared/audioif_trig.c` and the pole radius from a series.

A struck object — a drum head, a marimba bar, a bell, a wine glass — rings as
a sum of decaying sinusoids at frequencies that are not harmonics of anything.
That is the whole of what this node is. It was added for the acoustic drum kit
in audiocomponents, which has no other way to exist.

### Why it is not a chain of `audiobiquad.Biquad`

A band-pass at high Q *is* a resonator, so the obvious construction is N of
them in parallel behind `audioroute.Splitter` into `audiomixer.Mixer`. That is
the first thing this work tried, and it fails twice over. Measured, not
argued — the same shape of answer `audioverb`'s section above gives for its
own palette attempt:

- **`audioroute.Splitter` carries four taps** (`AUDIOROUTE_MAX_TAPS`), so a
  bank wider than four modes is not expressible without a tree of splitters
  feeding a tree of mixers. A drum wants eight to twelve; the dossier's
  citation for a cymbal wants between one and two thousand.
- **Every node in such a tree quantises to int16 on the way out.** A mode 40
  dB below the fundamental is then carried in about five bits, and what comes
  out of the mixer is dominated by the quantisation noise of the bank's own
  quiet modes. A six-mode 58 Hz kick at 48 kHz, identical excitation both
  ways: **spectral centroid 4113 Hz built from `Biquad` nodes against 73.8 Hz
  with the same six modes summed in float**, 0.86% of the energy above 1 kHz
  against 0.00%. The first is not a duller kick. It is noise wearing a kick's
  envelope.

So the sum has to happen before the quantiser, which means one node. Note that
the usual argument in this document — an argument added to audioif's copy of a
CircuitPython module would not exist on a stock board — does **not** apply
here: `audiobiquad` is already ours. The reason is the one above, plus the two
below, and together they make a different node rather than a wider one. The
name is the room.

### Decay time, not Q

`audioif_filter_f32.c` clamps Q to `AUDIOIF_FILTER_F32_MAX_Q` = 60, and says
why: an RBJ section's pole radius is `sqrt((1 - alpha) / (1 + alpha))` with
`alpha = sin(w0) / 2Q`, so alpha going to zero is a filter that rings for
ever. That rail is right for a filter and fatal for a resonator bank. A 20"
ride's partials ring for seconds, and `Q = pi*f*T60/ln(1000)` puts a 3 kHz
mode with a 3 s decay at **Q = 4093, sixty-eight times the cap**.

So a bank asks for `decay` in seconds. The bound is then explicit
(`AUDIOIF_MODAL_MAX_DECAY`, thirty seconds), it is stated in the unit the
caller is already thinking in, and it never approaches the coefficient corner
the cap was protecting against — the radius is computed directly rather than
arrived at through an alpha that has gone small.

`gain` is likewise the peak of the mode's impulse response rather than a
filter gain, which is what `b0 = gain * sin(w0)` buys: the impulse response of
`y[n] = b0*x[n] - a1*y[n-1] - a2*y[n-2]` is `r^n * sin(w0*(n+1)) / sin(w0)`.
A modal table read out of a paper goes in as published.

### The exponential is a series, for the trig's reason

`audioif_trig.h` explains why sine and cosine here are a polynomial rather
than libm: three platforms' `sin()` agree to within an ulp and differ in the
last place, and this port's rule is that one hash of one probe matches on all
three. The pole radius needs `exp(-ln(1000) / (decay * rate))` and inherits
the rule exactly.

`audioif_modal_exp_neg()` halves its argument until it is under an eighth,
evaluates seven Taylor terms by Horner, and squares back. Over `[0, 1/8]` the
first dropped term is `x^8/8! < 1.5e-12`, four orders below float32's epsilon.
Range reduction rather than a longer polynomial because the argument reaches
~0.9 at the short end (a one-millisecond decay on an 8 kHz graph) and a single
polynomial wide enough for that is both longer and worse at the small end,
which is where every real mode sits.

### Both state words or neither — the limit cycle

This is the part worth reading, because it was written the wrong way first.

`audioif_filter_f32.c` flushes each state word to exact zero on its own, below
`1e-20`, and that is safe there. Doing the same here produced a stable limit
cycle **above** the threshold. Measured at 220 Hz, 0.125 s decay, 8 kHz: the
state parked at ~2.1e-19 and was still there after two thousand blocks —
thirty-eight seconds of audio — with the output silent from block 30 onward.

The mechanism is visible in one line. With no input the recursion is
`s1' = -a1*y0 + s2`, `s2' = -a2*y0`, `y0 = s1`, and `-a1` is close to 2 for a
high-Q pole (1.9567 for that mode). The independent flush zeroes `s2` first,
because `a2 < -a1` makes it the smaller word; the very next sample computes
`s1' = 1.9567*y0 + 0`, nearly double what it was, and that refills `s2` from
it. The flush was not ending the tail. It was feeding it.

Zeroing the pair together cannot reach any state but the one where the whole
mode is off. That matters beyond tidiness: because a finished mode is *exactly*
zero, the process loop skips it on a test that cannot be wrong by a fraction of
an LSB, and a kit holding ten drums resident pays only for the ones sounding.

What found it is worth recording too. The trait that reads the **output**
reaching zero passed the entire time, because the output had rounded to zero
long before. Only the trait that reads the **state** could see it, and an own
node has no oracle that would have said anything at all. That is
`docs/correctness-standard.md`'s argument for traits, arriving on its own.

### Transposed direct form II, one corner further in

Same form and the same reason as the biquad beside it (audioif#64), except
that a resonator bank lives further into the corner than any filter does: a
long decay is precisely a pole held close to the unit circle for a long time,
and direct form I differences two nearly-equal large numbers with about two
decimal digits of `float` headroom left to do it in.

### What it costs

Per mode per frame per channel: three multiplies and two adds, plus the two
comparisons of the flush. A mode that has finished ringing costs one compare
and nothing else. Memory is two floats of recursion state per mode per
channel, plus three floats of mode table and three of coefficients per mode —
so a twelve-mode mono drum is 288 bytes of state and table, and a sixty-four
mode stereo bank is 2 KB.

### How it is verified

`tests/parity/modal_probe.py` in `verify_dsp`'s set, for the three-target
agreement `docs/correctness-standard.md` asks of an own node, and it prints
invariants as well as PCM — the block at which the tail reaches exact zero,
whether the bank is still sounding after its source has gone, and the peak of
a mode whose Q a filter would refuse. `tests/test_cpython_audiomodal.py`
carries eleven traits with their bars and the planted faults that break each,
including the limit cycle above, which is recorded there as what it was: a
real bug rather than a planted one.
