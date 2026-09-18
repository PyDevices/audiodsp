## Unreleased

- **A mixer voice mixes its source's buffer as it stands at mix time, on the
  CPython twin too.** `common_hal_audiomixer_mixervoice_play` keeps what
  `audiosample_get_buffer` handed back, and what it hands back is a pointer
  into the source node's own buffer — one buffer per node, not a queue. So
  anything that pulls the source between the fetch and the mix overwrites it,
  and the voice mixes the later block. The twin copied instead, and a class
  that settles a filter behind a voice it has already attached therefore
  rendered its first block differently on CPython than on desktop
  MicroPython, desktop CircuitPython and both boards: `Saturation`'s two
  `Bias`-off-centre patches came out `3b0e6b65f9c782d0` where every native
  leg said `1f9bcf50dc14a4ee`.

  `audiocore._AudioSample._publish` is the rule now — a node hands back its
  own buffer, refilled, with as many slots as the native rotates through (one
  for the nodes audioif wrote, two for the ported CircuitPython effects, for
  `Mixer` and for the synthesizer) — and `audiocore._borrow` is the in-graph
  pull that does not copy. `audiocore.get_buffer` still copies, on both
  targets, because it is the script-facing one.
  `tests/parity/mixer_borrowed_block_probe.py` is the gate, and
  [docs/correctness-standard.md](docs/correctness-standard.md) carries this as
  the third way one twin and three natives part company. audioif#89.

- **The soundtrack render gate runs again.**
  `tests/parity/capture_render_reference.py` had not been able to start since
  micropython-vst3 became mpvst. It looked for `<checkout>/soundtrack` and ran
  `tools/render_preview.py`; mpvst `ef0bed5` moved both composers beside their
  songs, so the soundtrack is `examples/soundtrack/` and the renderer is
  `examples/soundtrack/composer/preview.py` with `harness.py`. The script
  drives that, invoked as mpvst documents it, with the checkout defaulting to
  `../mpvst` (`--mpvst`; `--vst3` still answers). What it measures is
  unchanged: the old renderer and the new one are both thin shims over this
  repository's own `audiorender`, so the report is the same report and
  `normalize()`, `TRACK_LINE`, `SECTION_LINE` and `MASTER_LINE` read it as
  they did. One parser did need loosening — the renderer prints an elapsed
  time it gets by subtracting two wall-clock readings, and under WSL the clock
  resyncs backwards often enough that the figure comes out negative, which the
  normalizer's pattern did not match and which would have left an
  irreproducible number in a report that is diffed.

  `preview.py` loads an instrument the way the sidecar does, through the
  bundle's `mpvst_instrument_adapter`, so it wants MPVST installed. Rather
  than require a build, the gate points `MPVST_BUNDLE` at mpvst's own `lib/`,
  which is the directory an install is staged from and holds the adapters and
  nothing else. That is not a convenience: a staged bundle also carries
  *copies* of `audioinstruments` and `audioeffects`, and `harness.py` puts the
  bundle ahead of `PYTHONPATH`, so pointing at a real install would have made
  `--components-lib` inert and quietly graded a stale library instead of the
  one asked for. `MPVST_COMPONENTS_LIB` is set from the same directory, so the
  patches the composer reads and the packages the render imports cannot come
  from two different trees.

  A piece that cannot be rendered at all is now reported and the run carries
  on, rather than ending the gate. That is not hypothetical: two pieces build
  a `Chorus` the way `audioeffects` used to take one and fail immediately
  ([mpvst#8](https://github.com/PyDevices/mpvst/issues/8)), and stopping at
  the first of them left the other six unmeasured — which is the shape of
  failure this gate exists to avoid.

  The golden was **not** re-captured. It holds seven pieces from 2026-09-03
  and the soundtrack has grown since; whether that baseline is still the
  reference or should be re-taken against today's pieces and today's DSP is
  Brad's call, not an agent's (audioif#88).
- **`audioif_util.float32`, and the rule that a setting derived in Python
  goes through it.** A Python float is the interpreter's `mp_float_t` — a
  double here and on the desktop MicroPython, a **single** on every board and
  on a `MICROPY_FLOAT_IMPL_FLOAT` build — so `node.mix = 0.35` is two
  different numbers and the node renders different bytes on a board before
  its kernel is reached. `lib/audioif_util/` is `struct` and two functions:
  `float32(value)`, the round trip that is the identity on a single-precision
  target and a rounding on a double one, and `float32_bits(value)`, the exact
  way to print a float that two interpreters have to agree on.
  `docs/correctness-standard.md` carries the rule; it is also what
  audiocomponents#75 needs on the class side.

  What found it: `clean-build.yml`'s `unix-usermod (float-precision)` cell,
  where `verify_dsp` had six probes disagreeing with CPython. Four were the
  probes' own arithmetic and are fixed here. Three printed a gain reduction
  as `"%.6f"` whose **float32 bits were identical on all three targets** —
  MicroPython's single-precision formatter is not correctly rounded to seven
  significant digits, so that column compared formatters, not DSP; they print
  the bit pattern now. `granular_pitch_shift_probe` passed `mix=0.35`.
  `biquad_component_probe` derived its square wave from a Python float phase,
  so on a single-precision build it was filtering a *different waveform* (8
  frames flipped in mono, 96 in stereo); its material is integer arithmetic
  now, which is not byte-identical to the old float form on a double build
  either, so `golden/biquad_component.json` was re-captured. The CI cell's
  `--known-divergent` list drops from six probes to two.

  The two that are left are below the probes and each has its own issue:
  audioif#101, `synthio.Biquad` deriving W0 at `mp_float_t` width where the
  CPython extension calls the shared `double` `audioif_biquad_cp_w0()`; and
  audioif#102, a filtered `audiodelays.Echo` doing `echo * decay + sample` at
  `mp_float_t` width in C and in double in the twin. Both were proved by
  landing the CPython twin on the float build's bytes exactly, and both move
  board digests, so neither is folded in here (audioif#80).

- **`audioshaper.Waveshaper`'s own headroom is documented, and pinned by a
  trait test.** A curve that reaches the rails and a `post_gain` above about
  0.74 saturates the node's *own* output: the half-band decimator rings
  about a third past the rails on a hard edge, at the base rate, after the
  oversampling is already done, where no factor of it reaches. Measured on
  a hard-clipping table driven hard (x4, 1010 Hz, 48 kHz, bare node): the
  alias floor is flat through `post_gain` 0.80 and 13 dB worse by 0.90.
  `audioshaper.CLIP_HEADROOM = 0.74` names the ceiling — keep
  `post_gain * max(abs(curve))` at or below it for a curve that reaches the
  rails, and put the rest of the wanted level on a mixer voice after this
  node. No DSP changed: the docstring, `docs/upstream-diff.md` and a new
  `HeadroomTest` in `tests/test_cpython_audioshaper.py` say what a drive
  class (Distortion's second fix round) had already found and was carrying
  its own 0.74 ceiling for, uncredited. The constant lives on the CPython
  twin only — neither the MicroPython usermod's module globals nor the
  CircuitPython spike's export anything past `__version__`/`__revision__`
  and the two types, the same as `GROUP_DELAY_SAMPLES` beside it
  (audioif#99).

- **`audioshaper.SampleHold`**, a new node: a zero-order hold in which `num`
  source frames carry `den` new values, at a ratio that is exact. One frame
  in, one frame out, at the source's own rate, channels and bit depth; the
  accumulator is the remainder of `n·den` modulo `num`, so 26 040 Hz at
  48 kHz is 400/217 and stays 400/217 however long the render runs.
  `num == den` is a wire byte for byte, `latency` is 0, and `set(num, den)`
  moves the ratio mid-stream.

  It exists because the palette's sample-and-hold was a pair of
  `audiospeed.SpeedChanger` nodes, down by the hold ratio and up by its
  reciprocal, and that pair cannot be made reciprocal: the rate is 16.16
  fixed point and inverts exactly only at powers of two. Measured on the
  effects programme's shipped hold, the two rates multiplied to 0.9999947 at
  48 kHz — one sample late per 189 339 frames — and 1.0000108 at 44.1 kHz,
  one sample early per 92 708. A click read delay 1 at frame 256 and 2 at
  frame 196 608; at Mix 0.5 a steady 12 kHz tone swung 10.74 dB over a
  twelve-second render, which is a slow flange on a setting nobody was
  touching. The rate form was **not** added to `SpeedChanger`: that module is
  a CircuitPython port, an argument added to audioif's copy would not exist
  on a stock board, and `audiospeed` is byte-identical to what it was
  (audioif#97).

- `audiospeed.SpeedChanger` **carries its phase across a source buffer**.
  Upstream zeroes the accumulator every time it takes a new buffer, so what the
  node renders depends on how the node above it chunks its output: the same
  hold over 64- and 256-frame buffers differed in 1947 frames of 2048. A
  sample-and-hold built from a pair therefore drifted (73 codes over 65536
  frames at 48 kHz, 430 at 44.1) and spread its images instead of placing them
  — a 1 kHz image under a 7 kHz tone held at 8 kHz read −39.9 dB where a
  zero-order hold puts it at −0.22. All of it is 0 now, and the render matches
  `source[(((n·up)>>16)·down)>>16]` exactly. A rate that divides the buffer
  length was always right, which is why the parity gate never saw it.
  `reset_buffer` still starts the stream over. Upstream still restarts, so this
  is a named departure — `docs/upstream-diff.md`, report drafted (audioif#91).

- `audiospeed`: the 16.16 rate **rounds** now instead of truncating. Upstream
  casts, so a float landing a hair below its Q16 neighbour lost a whole LSB —
  `1/1.0000000000000004` came back as 65535/65536, and a `SpeedChanger` pair
  asked for "the hold rate = the running rate" was not an identity at 44.1 kHz
  (27666 codes of error on a full-scale tone; 48 and 22.05 kHz happened to land
  on 1.0 and looked fine). `Resampler`'s bound ratio rounds too: 48000/44100 is
  71332, not 71331. Upstream CircuitPython still truncates, so this is a named
  departure — `docs/upstream-diff.md`, report drafted (audioif#92).

- `audioroute.Splitter` lost the head of any block bigger than its
  8192-frame ring. `audiocore.get_buffer` takes no length, so a source hands
  back what it has — a `RawSample` over a table returns the whole table — and
  writing 9600 frames lapped every cursor including the one about to read:
  1408 frames gone, and a seam at 8192. It writes in ring-sized pieces now
  and keeps the remainder. Lapping a *laggard* tap is unchanged and still
  deliberate. `audioroute.RING_FRAMES` is exposed for callers who need the
  number (audioif#87).

- `audiomixer`: a mixer voice looping a sample too short to fill one packed
  32-bit word — a one-frame mono `RawSample` is two bytes — never returned
  from `get_buffer` on the native builds. `play(loop=True)` now raises
  `ValueError` instead, and the mix-down carries a backstop that stops such a
  voice rather than spinning on it. Refused rather than padded: padding a
  one-frame loop halves its loop rate, and would have to copy a buffer
  `RawSample` deliberately does not own. Upstream CircuitPython still spins
  here (audioif#85). `MixerVoice.loop` gains a property on the CPython target,
  which had only the `play(loop=)` argument.

- `audiomodal.Bank`: a bank of resonators, which is what a struck object
  is. N two-pole resonators fed one excitation, summed in float and
  quantised once, parameterised by 60 dB decay time rather than Q. It is
  a node rather than a chain of `audiobiquad.Biquad` because the sum has
  to happen before the quantiser: a six-mode 58 Hz kick built out of
  `Biquad` nodes measures a 4113 Hz spectral centroid against 73.8 Hz
  summed in float, and `audioroute.Splitter` stops at four taps anyway.
  Reaches CircuitPython through the additive path. Shared C in
  `src/shared/audioif_modal.c`.

- `audiomath.remix_s16`: interleaved native-endian s16 1↔2 channel convert
  (stereo frames to (L+R)/2, or a mono sample duplicated). Shared C in
  `src/shared/audioif_remix.c`, bound on the usermod and the CPython
  extension. Not a graph node — `audiomixer.Mixer` still requires sources
  that already match `channel_count`.

## v0.4.0 (2026-09-10)

Brings `synthio` and `audiomixer` up to CircuitPython 10.3.0, which moves
rendered audio: a pan control now works the other way round, and level changes
wait for a zero crossing. The comparison build is now made at the same version
and voice ceiling this port ships, which is what found those two changes and
retires the last ceiling deviation. See
[docs/correctness-standard.md](docs/correctness-standard.md).

### Breaking

- A released node raises `ValueError`, not `RuntimeError` on the CPython target.
  Same type and message as the native builds now. Code catching `RuntimeError`
  around a teardown path should be changed to `ValueError`. (#73)
- `synthio`, `audiomixer`: `panning > 0` attenuates the **left** channel.
  Upstream flipped `synthio.Note` to match `audiomixer.Mixer`; both were
  reversed here, so the direction of every pan is inverted. A patch or a
  composition that sets `panning` should have its sign flipped.
- `synthio`, `audiomixer`: a level, amplitude or pan change now waits for a zero
  crossing, so it cannot click. Delayed by at most one block.
- `synthio`: a voice at envelope level 0 renders nothing, as the native builds
  do. Previously it kept sounding at its previous loudness for up to a block.
  (#78)
- `synthio.Biquad`, and so `audiofilters.Filter`, runs CircuitPython's Q15
  arithmetic on all three targets. `audiobiquad` keeps the widened kernel. The
  deviation had been applied on one target only, 11 LSB apart by the eighth
  sample of an 800 Hz low-pass. (#77) — **use `audiobiquad` below 100 Hz**, see
  [which filter to reach for](docs/upstream-diff.md#which-filter-to-reach-for)
- `audiobiquad`: transposed direct form II. 7 of 56 f0/Q cells outside 0.05 dB
  becomes 1, at no measurable cost. (#64)

### Added

- Every module of ours reports `__version__` and `__revision__`, so a firmware
  can name the audioif it was built from. A published wheel reports `unknown`
  for the revision -- it is built from an sdist with no `.git`, and its
  `__version__` already names it exactly. (#55)
- `audiodelays.Flanger`, `audiodelays.GranularPitchShift`, `audiospeed.Resampler`
  and the `audiofilters` filter chain on MicroPython, so all three targets carry
  CircuitPython 10.3.0's nodes. (#74)
- `audiodelays.Echo.filter`, `audiofreeverb.Freeverb.pre_filter`/`post_filter`.
- `audiodynamics`: `gain_smooth_ms`, a one-pole on the computed gain (#61), and
  `feedback_gain_corrected`, so a feedback loop lands on its ratio (#62). Both
  default off.
- `audioroute.MidSide`, and `audiomath.SubOctave` as an analog octave divider.
- `deinit()` on the twelve node types that had none, plus the deinitialised guard
  on the one funnel every pull goes through. (#58, #59, #60, #63)

### Fixed

- `audiodynamics`: the ESP32-P4 and ESP32-S3 rendered the transient attack path
  differently. Fused multiply-add, forbidden in that kernel; the two boards now
  agree byte for byte. (#66)
- `audiodynamics`: `reset()` clears the key filters, so a reset node is a fresh
  one. (#56)
- `audioconvolve.Convolver.latency` reports 0 when nothing is loaded. (#44)
- `audioecho.FeedbackDelay` renders mono correctly when a source block is short.
- `audioroute.Splitter` can be released, and releasing it releases its taps.
- `audiodelays.Flanger`: the wet interpolation overflowed `int32` on full-scale
  material. Upstream's still does. (#76)
- `audioeffects.Phaser` and `audioeffects.LowPass` return to exact silence.
  `audiofilters.Phaser` still holds DC. (#23, #36)
- The comparison build was made at a different CircuitPython version and voice
  ceiling than this port ships, so nothing above the mix-down knee could agree
  with it. It is now built at 10.3.0 at our own ceiling, and the 14 → 64
  deviation is retired rather than documented. (#27, #31)
- `biquad_component_probe` was graded against its own stored digest, so it could
  not see two of our own targets disagreeing. `verify_dsp` now compares the
  three interpreters.
- Nothing held the MicroPython and CircuitPython bindings to the same surface;
  three changes drifted in one day. `tests/test_binding_parity.py` does. (#75)
- Stored digests were a reference of record, and a digest can be re-blessed.
  Replaced by numeric traits with planted faults for every own node. See
  [docs/correctness-standard.md](docs/correctness-standard.md).

### Documentation

- [docs/correctness-standard.md](docs/correctness-standard.md): what audioif is
  held to, in one page.
- [docs/upstream-diff.md](docs/upstream-diff.md) records what CircuitPython's
  Q15 biquad does below 100 Hz, where `b0` rounds to zero, and which filter to
  reach for.

## v0.3.0 (2026-09-09)

- Give the CPython target CircuitPython 10.3.0's new delay, speed and filter-chain nodes.
- Enable audiospeed on coverage, with the warning downgraded for its objects only
- Neither audiospeed nor audiofilewriter can be reached on unix coverage
- Compile audiospeed and audiofilewriter into the unix coverage build
- --dry-run now checks the anchors it claims it would insert after
- --status exits nonzero when the tree is not in the applied state
- apply_cp_patches: the mid/side kernel is listed in the CircuitPython variant Makefile too
- flake8: .deps on its own line
- parity: the mid/side probe runs on MicroPython too, and flake8 skips the vendored ulab
- build: every merged source block in micropython.mk opens its own SRC_USERMOD_C list
- build: the ladder's sources get their own SRC_USERMOD_C block again
- mpaudio_modules: the own-module roll call names all six, not four
- Effects Phase 1 integration: re-capture the DSP golden with the oracle
- Smoke audioshaper in the clean-build workflow, and name it in the porting plan
- Record audioshaper: what the palette could not do, and what the node measures
- apply_cp_patches.sh carries audioshaper into a CircuitPython tree
- porting-plan: the palette lacked a frequency divider, not just a stream multiply
- The hysteresis half-width is a coercivity, not a post-gain number
- Cover MidSide's surface where Dynamics' and Splitter's is covered
- Give audiomath an analog octave divider, so a sub-octave is not a pitch shift
- Reading a coefficient should not move an LFO along
- Record MidSide where the other four own nodes are recorded
- Give audioroute a mid/side matrix, so a drive can work on the middle of an image
- Write down what audiobiquad is for, and where the numbers came from
- A parity probe and a golden for audioshaper.Waveshaper
- Cite the Freeverb kernel by line, and say precisely where its all-passes sit
- audiobiquad reaches CircuitPython too, additively
- The tilt converts dB through expf, not powf, and the plan names tier 7's sixth
- audioshaper.Waveshaper: the curve arrives as data, and the shaping happens above the sample rate
- Record audioverb in upstream-diff, README and the module lists, and test it
- audiobiquad on MicroPython: the same two kernels, the same block reads
- Golden the ladder, and assert in numbers that the bytes are a ladder
- audioverb reaches CircuitPython too, through the additive path
- audioverb.Tank: a reverberation tank whose network comes from Python
- Give the palette a filter whose tail actually reaches zero
- audioladder on the other three targets: MicroPython, and the CP tree
- A transistor ladder, solved rather than delayed, on the CPython target
- audiodynamics: record the twenty-one options as a deviation, with the measurements
- audiodynamics: a parity probe for the twenty-one options, and its golden
- audiodynamics: the new options on all four targets, and an external key input
- audiodynamics: twenty-one additive options on the shared dynamics kernel
- Say what the loop shift costs in repeat time, in all three places it is documented
- Record the four FeedbackDelay options: what they were asked for, what they measure
- A parity probe and a golden for the four new FeedbackDelay options
- Four options on audioecho.FeedbackDelay, all off by default
- upstream-diff: the audible half of the ceiling raise is refused voices, not the knee
- upstream-diff: the voice ceiling is 64 where CircuitPython builds 14
- Re-pin the CircuitPython oracle to the bytes it has, with the provenance the relink never recorded
- Clean the single-precision narrowing, so the float CI cell can run at -Werror
- A fifth parity gate whose material crosses the mix-down knee
- build: micropython.cmake finds ulab in .deps/ or a sibling, so the standalone claim holds on CMake ports
- packaging: numpy is the [render] extra, so a bare install is honest about audiorender
- Move the attribution out of LICENSE so GitHub can detect MIT
- PR #11289: the reply is posted
- PR #11289: draft reply to relic-se, with the measurement behind it
- synthio: raise the voice ceiling from 14 to 64
- tests: assert the five voice-ceiling sites agree, and pin the oracle's bytes
- parity: re-capture the vst3 render reference against audiocomponents at the v0.2.0 core
- lib: delete audioinstruments and audioeffects -- they live in audiocomponents

- audiodynamics: twenty-one additive options and an external key input, for the effects program (#38)
- audioroute: MidSide, a zero-latency mid/side matrix, exact identity at width=1
- audioshaper: a new audioif-own module -- a table waveshaper, oversampled x2/x4/x8, with an off-by-default hysteresis option
- packaging: numpy is the `render` extra, so a bare install says honestly what audiorender needs

## v0.2.0 (2026-09-03)

- test-cpython: the matrix comment no longer names a validator that left with the components
- synthio: one polyphony ceiling, 14, on every build path
- docs: the instrument and effect libraries live in audiocomponents
- packaging: nothing here freezes or ships the component packages
- publish: audioif no longer publishes the component packages or MIP
- tests: component tests and the instruments gate go to audiocomponents
- ci: bump the actions group across 1 directory with 2 updates (#10)
- README: the standalone claim holds for Make ports; CMake ports still need a sibling ulab
- verify_effects: re-accept aarch64 under the byte-checksum format, from the CI report
- parity: the acceptance gate now sees byte order, not just a sum
- parity: the effects gate hashes bytes, not just their sum
- parity: the biquad gate now covers Note.filter, Q and A, and sees byte order
- audiomixer: a source that promises data and delivers none must not hang
- parity: retire the 40 instruments af837de changed on purpose
- parity: re-capture 19 cpython digests stranded by b420dac
- AGENTS.md: the stored parity digests stay (#26 reversed) and why
- AGENTS.md: two kinds of golden, two rules
- parity: verify_acceptance no longer discards the caller's PYTHONPATH
- docs: fix the four audit defects in the readability batch, and my own aarch64 note
- parity scripts: drop hardcoded cmods paths
- apply_cp_patches.sh: drop the cmods-specific sibling fallback
- docs: reword cmods-workspace mentions to a generic term
- readme: document a standalone MicroPython build recipe
- readme: add Installation section to audiorender
- readmes: add Installation sections to audioeffects and audioinstruments
- readme: restructure for scannability, add direct audioif install line
- docs: note where the component docs will live
- spec: record why patch values are integers, and that it is settled
- instruments: log-map time and filter-frequency macros, and re-derive patch 0
- build: the excess voices are REFUSED, not stolen - fix both comments
- cpython: give a released note's channel back before its tail ends
- wheels: record what the ARM lane found, and the rule it settled
- verify_effects: per-architecture exact baselines, no tolerance anywhere
- verify_effects: report WHAT differs, not just that a hash moved
- wheels: add Linux aarch64 (CI-proven), keep musllinux skipped
- _support: asym wavetables crash on ulab -- np.abs does not exist there
- cpython: make envelope reassignment live, matching the oracle
- synthio: implement ring modulation on the CPython target
- upstream: synthio re-press fix submitted as circuitpython#11289
- PR: describe the test we actually ship, not the two-note bisection
- PR: cut the body to house style, 163 words to 75
- PR draft: record the independent mechanism check against upstream main
- check_attribution: drop an unused import that would have reddened CI
- Attribution: catch the 12 my first sweep missed, and guard it in CI
- Prepare upstream PR for synthio note re-press dropping at envelope level 0
- upstream-diff: we filed five and four are merged, not 'none has been filed'
- Restore upstream copyright attribution to every ported file
- synthio: a re-pressed finished note is a new hit, not a swell
- synthio: oracle-exact press semantics on CPython; Note.filter cascades
- ci: let Dependabot watch this repo's GitHub Actions
- upstream ledger: band-edges reframed and posted on #11269; PR waits on maintainer appetite
- upstream ledger: all four fix PRs merged upstream (#11275-#11278)
- upstream ledger: four fix PRs filed (adafruit/circuitpython #11275-#11278); band-edges awaits its design framing

## v0.1.1 (2026-08-31)

- release chain: publishing-v7 -> publishing-v8

## v0.1.0 (2026-08-31)

- Prepare upstream PRs for dds-oscillator-off-by-one, distortion-soft-clip-union, biquad-reset
- docs: prepared upstream PR for peaking-eq-sign (#11265) -- patch and paste-ready body
- docs: the two-layer sound guarantee -- the CircuitPython-parity core is the floor, the components evolve
- docs: the sound-stability contract on all three front pages
- release chain: pin publishing-v7, expect 21 wheels (macOS arm64 joins)
- docs: racks are delivered — shipped-status recordings and the 46-class count everywhere
- audioeffects: effect racks — Rack mechanism plus ShimmerHall and AirSpace, ported from micropython-vst3
- setup.py: -ffp-contract=off on macOS so arm64 wheels match the parity oracle
- CI: turn on the macOS lane — test matrix, wheel-build proof, arm64 config
- CI: add a single-precision cell to the clean build, complete the smoke imports
- patches: give the Adafruit_MP3 patch its provenance header and fix the dead path
- audioinstruments README: match the documented API to the implemented one
- docs: correct stale claims -- publishing pin, wheel exclusions, Android, effect count, parity locality
- README: fix the quickstart install and the wheel-contents claim
- docs: record the specified-but-unshipped component surface honestly
- CI: run every tests/test_*.py, not just test_cpython_*
- Fix MultiTapDelay tap_levels type mismatch on MCU float builds

## v0.0.5 (2026-08-29)

- Adopt publishing-v6 (MIP second-publication race fix)
- clean-build: USER_C_MODULES is the parent directory of the module
- Standalone builds: pinned deps, owned patch queue, clean-build CI
- Declare the CircuitPython oracle pin in a checked-in file

