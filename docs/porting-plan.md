# Porting CircuitPython's audio system to MicroPython — plan

## Goal

Full parity with CircuitPython's audio system as a `USER_C_MODULES` MicroPython
usermod tree, targeting **mcu, unix, and windows** ports. In scope: the
complete DSP stack (`audiocore`, `audiomixer`, `audiodelays`, `audiofilters`,
`audiofreeverb`, `audiospeed`, `audiomp3`, `synthio` including `MidiTrack`)
plus output devices with CircuitPython `play()` semantics. `ulab` is a cloned
dependency, not a port (see below). Acceptance target: todbot's
[`synthtools`](https://github.com/todbot/CircuitPython_SynthTools) runs
unmodified.

> Plan revision note: v1 of this plan scoped only `synthio`, skipped
> `MidiTrack`/`audiocore`/`audiomixer`, and bridged output into
> `audiodev.PCMOutput`. All three calls are reversed here — see
> "Architecture" for why the audiosample pull protocol must be ported as the
> spine, and "ulab" for a correction (v1 claimed `synthtools` needs no ulab;
> `synthtools/waves.py` imports `ulab.numpy`).

## Verified facts this plan rests on

- **The DSP layer is provably hardware-independent.** CircuitPython's own
  unix `coverage` variant (`ports/unix/variants/coverage/mpconfigvariant.mk`)
  compiles every module listed above with **no** audio output device and no
  `audio_dma` — the workspace's `bin/circuitpython` (10.2.1) already contains
  all of them. `audio_dma` coupling exists only in `ports/*` output drivers,
  which we are replacing anyway.
- **`bin/circuitpython` is our parity oracle.** Any DSP test script can run
  on both interpreters and the rendered PCM compared sample-for-sample. This
  is the backbone of the test strategy.
- **ulab needs no porting.** CP's `extmod/ulab` submodule IS upstream
  [v923z/micropython-ulab](https://github.com/v923z/micropython-ulab)
  (pinned 6.5.2), which ships MicroPython usermod build glue at
  `code/micropython.mk` / `code/micropython.cmake`. Clone as a sibling in
  the parent workspace, pin to CP's revision for parity.
- **CP's protocol machinery maps onto mainline.** `py/proto.h` (60 lines
  total with `proto.c`) implements named protocols; mainline MicroPython has
  the same concept as the `protocol` type slot (used by `mp_stream_p_t`).
  The audiosample protocol ports as a slot-based protocol struct plus a
  small compat header.
- **Licenses are compatible.** CircuitPython shared-bindings/shared-module
  are MIT with per-file SPDX headers; this workspace is MIT. Carry the
  upstream copyright headers forward per-file. (`lib/mp3` for `audiomp3`
  has its own license — verify before vendoring, see open questions.)
- **Module sizes (10.2.1, bindings + shared-module, LOC):** synthio 4168,
  audiodelays 3231, audiofilters 2242, audiocore 1239, audiomixer 1155,
  audiomp3 923 (+ vendored `lib/mp3`), audiofreeverb 750, audiospeed ~600.
  Roughly 14k lines of portable C to port, plus compat shim and new output
  devices.

## Architecture

### The audiosample pull protocol is the spine

Everything in CP audio composes through one small protocol
(`shared-module/audiocore/__init__.h`, ~90 lines): `reset_buffer` /
`get_buffer` / a base struct carrying sample rate, bits, channels. Mixer
pulls from its voices, every effect pulls from its `source`, outputs pull
from whatever is `play()`ed, and `Synthesizer` *is* an audiosample. Porting
this protocol first — on mainline's `protocol` type slot — is what makes
every subsequent module a mostly-mechanical port and keeps them composable
exactly as upstream (`effects chained into Mixer into an output`, etc.).

The v1 idea (push rendered PCM into `audiodev.PCMOutput`) inverted this:
audiodev is push/write()-based and Python-level. It survives only as an
optional adapter (an object that pulls any audiosample and `write()`s into
any audiodev sink), useful for wasm/web output later — it is not the parity
surface.

### Module tiers and dependencies

```
tier 0  cp_compat: proto slot glue, py/enum.h shim, objproperty/attr shim,
        mp_arg_validate_* + translate() replacements, background-callback stub
tier 1  audiocore: audiosample protocol + RawSample + WaveFile   (pure, stream I/O)
tier 2  synthio: block layer, Math, LFO, Biquad, Note, Synthesizer, MidiTrack
        (Synthesizer/MidiTrack implement the tier-1 protocol; effects reuse
        synthio's Biquad/BlockInput, so synthio ports before the effects)
tier 3  audiomixer: Mixer, MixerVoice
tier 4  effects: audiofilters, audiodelays, audiofreeverb, audiospeed
tier 5  audiomp3 (vendored lib/mp3 decoder; license check first)
tier 6  outputs (new code, not a port — see below)
tier 7  audiodynamics, audioroute, audiomath, audioecho, audioshaper,
        audioladder, audioconvolve, audiobiquad, audioverb,
        audiomodal: native, NOT ports
tier 8  lib/: pure Python built on the tiers above (audiorender here;
        audioinstruments and audioeffects have since moved to audiocomponents)
dep     ulab: cloned sibling in the parent workspace, pinned to CP's 6.5.2
```

### Tiers 7 and 8 run the other way round

Everything through tier 6 answers "what does CircuitPython have that
MicroPython and CPython do not". Tiers 7 and 8 answer the opposite question,
and their source is micropython-vst3 rather than CircuitPython:

- **tier 7** is `audiodynamics` (compressor, limiter, downward expander, gate,
  transient shaper, with a high-passed detector for de-essing) and
  `audioroute.Splitter` (fan one stream out to parallel branches over a shared
  ring).
  Both lived in micropython-vst3's `vstaudio` usermod, which meant no
  application outside that plugin could use them and half of its own effects
  library could not be exercised offline at all.

  `audiomath`, `audioecho`, `audioshaper`, `audioconvolve`,
  `audioverb`, `audiomodal` and `audioroute.MidSide` have no ancestor at
  all — nothing upstream and nothing in the engine does any of the seven. Nothing
  multiplies two *streams*, which is what ring modulation needs and what an
  LFO at block rate cannot reach. Nothing puts a filter inside a delay's
  feedback loop, which is what separates a tape echo from a delay with a tone
  control. Nothing lets you supply the *curve* of a nonlinearity, or applies
  one above the sample rate, so every drive circuit was four fixed shapes at
  the base rate and the harmonics they make past Nyquist fold back onto the
  signal. And nothing transforms anything at all, so nothing can apply a
  measured impulse response — the one effect on the catalogue the rest of the
  palette genuinely cannot approximate. And nothing reaches what is already
  *between* the speakers: `Mixer`'s `pan` places a source, but no combination
  of pans collapses a stereo pair to mono or pushes its sides out, which is
  what a stereo drive needs in order to keep its image. `audiomath` gained
  a second class in the effects program's Phase 1 for the same kind of
  reason: nothing divides a
  *frequency* either, so an octave down could only be granular (smeared) or a
  ring modulator (locked to nothing), never the flip-flop divider a pedal
  actually contains. And `audiofreeverb` reverberates with
  a network compiled into it, so nothing could re-cut a reverberation tank per
  character or modulate a line inside its loop — nor could a cycle of palette
  nodes stand in, the pull graph having none (the first `get_buffer` of one
  raises `RecursionError`).

  CircuitPython has no equivalent to
  any of them, so `apply_cp_patches.sh` adds them to a CircuitPython tree —
  the only direction in this repo where CircuitPython is the recipient. See
  `docs/upstream-diff.md` for what changed in the moves and what was kept.
- **tier 8** is pure Python under `lib/`. `audioinstruments` — 53 classic
  synthesizers, keyboards and drum machines written entirely in `synthio`,
  no samples — and `audioeffects` — 46 effect classes built on the tiers
  above — were moved out of micropython-vst3 here so they are not tied to a
  VST host, and have since moved once more, to the
  [audiocomponents](https://github.com/PyDevices/audiocomponents)
  repository, which develops and publishes them as their own distributions.
  What remains here is `lib/audiorender/`, the tier above those two: a
  composition — tracks, a tempo map, notes, automation, sections — rendered
  offline to a mixed master and the level report a render is judged by. It
  is the one part of this repository written for a desktop rather than a
  board (numpy throughout, the whole song in memory), so it ships in the
  wheel and `manifest.py` never freezes it. Loading a track's sound stays
  the caller's job: `render()` asks for a `voice_for(track, clock)` and only
  requires `deliver()` and `pull_frames()` back, which is what lets
  micropython-vst3 keep driving its own script loader through it.

Tier 7 has its own oracle, and it is the micropython-vst3 checkout rather
than `bin/circuitpython`: `tests/parity/verify_dsp.py`, which never writes
to that tree. The instruments' gate, `run_instruments_parity.py`, went to
audiocomponents with the instruments.

micropython-vst3 has since been cut over to import these packages instead
of carrying its own copies, so that checkout is no longer an independent
oracle for them: it *is* them. What still holds it honest is
`tests/parity/golden/vst3_render_reference.json`, captured from the
plug-in's six soundtrack pieces before the cutover and compared with
`tests/parity/capture_render_reference.py --verify` — whose interpreter
now needs `audioinstruments` and `audioeffects` installed from
audiocomponents, since they are no longer in this tree. Re-capture it after
any DSP change here, or it stops meaning anything.

### Output devices (the only new design work)

CP's `audioio.AudioOut` / `audiobusio.I2SOut` / `audiopwmio.PWMAudioOut` are
per-port DMA drivers — none port directly. Parity means reproducing their
*semantics*: `play(sample, loop=False)`, `stop()`, `pause()`/`resume()`,
`playing`, `paused`, pulling via the audiosample protocol.

- **Desktop (unix, windows):** one native `AudioOut` backed by SDL2's audio
  callback (SDL2 already lives in this workspace for `displayif`/usdl2; the
  callback thread pulls `get_buffer` on demand — the closest desktop
  analogue to CP's DMA-refill model). This is also what CP itself lacks on
  unix, so here we exceed the oracle: DSP correctness is tested against
  `bin/circuitpython`, playback by ear/WAV capture.
- **MCU:** a pump over `machine.I2S` non-blocking mode (its completion
  callback plays the role of the DMA-refill IRQ), exposed with the same
  `play()` API. Native per-SoC DMA drivers are a possible later
  optimization, not v1.
- **Adapter (optional):** audiosample → `audiodev.PCMOutput.write()` for
  wasm/web and any host audiodev already supports.

Teardown follows workspace rules (`displayif/AGENTS.md`): any output owning
host resources (SDL device, I2S instance) registers soft-reset teardown and
supports idempotent `deinit`.

### What "full parity" does NOT include (v1)

- `usb_midi` — USB device stack (TinyUSB) coupling; MIDI parity in v1 is
  `synthio.MidiTrack` (in scope, tier 2) plus pure-Python `adafruit_midi`
  over any stream/UART, which needs no port. Revisit against mainline's
  `machine.USBDevice` later.
- `audiobusio.PDMIn` (input capture) — hardware-specific; not needed for
  the playback/synthesis parity goal.
- `rotaryio`/`keypad`/other synth-adjacent CP hardware modules.

## Target layout

```
audiodsp/
  AGENTS.md
  README.md
  LICENSE
  micropython.cmake            # aggregates src/*/micropython glue for CMake ports
  micropython.mk               # ditto for Make ports (unix, windows)
  src/
    cp_compat/                 # tier 0 (proto, enum, properties, validation)
    audiocore/                 # tier 1 (protocol + RawSample + WaveFile)
    synthio/                   # tier 2
    audiomixer/                # tier 3
    audiofilters/ audiodelays/ audiofreeverb/ audiospeed/   # tier 4
    audiomp3/                  # tier 5
    audioout/                  # tier 6 (SDL2 desktop, machine.I2S pump, audiodev adapter)
  docs/
    porting-plan.md            # this file
    upstream-diff.md           # running log of any behavioral deltas vs CP (target: empty)
  tests/
    parity/                    # scripts runnable on BOTH interpreters; runner diffs PCM output
    test_*.py                  # MP-only unit tests (protocol, outputs, lifecycle)
```

Build-glue detail: the parent workspace's CMake aggregator finds
`*/micropython.cmake` at depth ≤ 3, so its ulab sibling's own
`code/micropython.cmake` is picked up as-is; but
Make ports glob only `$(USER_C_MODULES)/*/micropython.mk` (depth 1), so this
module's root `micropython.mk` must `include ../ulab/code/micropython.mk`
(or a one-line wrapper) for unix/windows builds. For a *standalone* CMake
consumer there is no aggregator, so `micropython.cmake` carries the same
`.deps/ulab` → `../ulab` search as the Make flavor and stands down (leaving
`cmods/ulab` to the aggregator) whenever `CMOD_DIR` is set — ulab's own
`add_library(usermod_ulab)` is unguarded, so including it twice is an error.

## Phases

1. **Scaffolding** *(done)* — tracked dir, plan.
2. **ulab dep + tier 0 + skeleton** *(done)*. `ulab` cloned as a parent
   workspace sibling and pinned to CP's exact revision (`1d3ddd8`); both
   build flavors wired;
   `import ulab.numpy` works on unix MP (needed
   `MICROPY_MODULE_BUILTIN_SUBPACKAGES`, off by default on mainline — see
   micropython.mk). `cp_compat` built: `argcheck`, `enum`, `objproperty`,
   `context_manager_helpers`, `proto`, `util` — each individually checked
   against mainline MicroPython before porting, not assumed missing.
   8 module skeletons registered.
3. **Tier 1 `audiocore`** *(done)*. Protocol (`audiosample_get_buffer` and
   friends) + `RawSample` + `WaveFile` all ported and passing oracle-diff
   tests against `bin/circuitpython` (`RawSample`: full property/deinit/
   context-manager/PCM diff, byte-for-byte match; `WaveFile`: PCM verified
   against Python's own `wave` module instead, since CP's own unix build
   can't open a WaveFile at all — see docs/upstream-diff.md). Found and
   fixed a real gap along the way: mainline has no code path that invokes a
   declarative property on a *native* type (CP patches this into its own
   core `py/runtime.c`); every ported type needs an explicit
   `attr, cp_compat_attr` slot instead — see docs/upstream-diff.md,
   "Property invocation needs an explicit attr slot." This applies to every
   remaining tier, not just audiocore.
4. **Tier 2 `synthio`** *(done)*. Full engine ported in dependency order
   (block layer → Math/LFO/Biquad → Note → Synthesizer → MidiTrack), all
   oracle-diffed against `bin/circuitpython` 10.2.1 byte-for-byte: `Math`
   (all operations), `LFO` (multi-tick phase accumulation), `Biquad`
   (filter coefficients), `Envelope`, and — the acceptance case — a full
   `Synthesizer` note press → attack → decay → sustain → release → PCM
   render cycle, plus a `MidiTrack` melody, both bit-exact against upstream.
   `synthio.from_file()` got the same portable-stream adaptation as
   `WaveFile` (see docs/upstream-diff.md); everything else is a mechanical,
   oracle-verified port. One new compat piece needed: `cp_compat/namedtuple`
   (CP's `py/objnamedtuple.h` extension for building a const namedtuple
   type at compile time, used for `Envelope`) — same "verify against
   mainline before porting" discipline as tier 0/1's shims. Full regression
   clean (`build_interpreters.sh`, LVGL smoke test, all 8 module stubs)
   after every change.
5. **Tier 3 `audiomixer`** *(done)*. `Mixer` + `MixerVoice` ported
   (`src/audiomixer/`), oracle-diffed against `bin/circuitpython` 10.2.1
   byte-for-byte across every branch of the mixdown engine: mono→mono,
   mono source upmixed into a stereo mixer with panning, 16-bit signed and
   8-bit unsigned sample formats, single- and multi-voice mixdown
   (verbatim-copy vs. add-mixed paths), looping, and `level`/`panning`
   driven by a `synthio.LFO`/`synthio.Math` block input. Two deviations,
   both mechanical (see docs/upstream-diff.md): the `#if CIRCUITPY_SYNTHIO`
   guard around block-input level/panning is unconditional here (this port
   always has synthio); ARM CMSIS DSP intrinsics dropped for the portable C
   fallback unconditionally (numerically identical, avoids a CMSIS
   dependency this port doesn't vendor -- relevant for phase 9 mcu
   builds). Full regression clean (`build_interpreters.sh`, LVGL smoke
   test, all module imports) after every change.

6. **Tier 4 effects** *(done)*. All nine types across four modules ported
   (`src/audiospeed/`, `src/audiofreeverb/`, `src/audiofilters/`,
   `src/audiodelays/`): `SpeedChanger`; `Freeverb`; `Filter`, `Distortion`,
   `Phaser`; `Chorus`, `Echo`, `MultiTapDelay`, `PitchShift`. All
   oracle-diffed byte-for-byte across mono/stereo, 8/16-bit signed/
   unsigned, looping, block-input-driven (`synthio.LFO`) parameters,
   mid-stream parameter changes, and stop/tail-drain behavior. `SpeedChanger`
   needed a temporary one-off oracle rebuild (`CIRCUITPY_AUDIOSPEED` isn't
   in the canonical unix coverage build); every deviation and verbatim-kept
   upstream quirk (a union type-pun, a `memset` fill-value truncation, an
   inconsistent `single_channel_output` check, two custom `__exit__`
   overrides) is catalogued in docs/upstream-diff.md. Full regression clean
   (`build_interpreters.sh`, LVGL smoke test, all module imports) after
   every change.
7. **`synthtools` end-to-end (unix)** *(done)*. **Primary acceptance
   milestone.** Vendored todbot's `CircuitPython_SynthTools` unmodified
   (`tests/vendor/synthtools`, MIT); a new acceptance script
   (`tests/parity/synthtools_acceptance.py`) drives its `SubtractiveSynth`
   and `BasslineSynth` engines through a real `Synthesizer` + `Mixer` +
   `EffectsChain` (tier 4's `Filter`/`Distortion`/`Echo`), oracle-diffed
   byte-for-byte against `bin/circuitpython`. Found and fixed a genuine
   port bug this way, not just confirmed the port: `synthio`'s
   `CIRCUITPY_SYNTHIO_MAX_CHANNELS` (max concurrent Notes per
   Synthesizer) defaulted to CP's own conservative 2 with no build
   override, while the oracle's unix `coverage` variant is itself built
   with 14 -- silently truncating polyphony past 2 concurrent Notes (any
   two-oscillator/detuned patch with two held notes already needs 4).
   Fixed in `micropython.mk` for unix/windows only; mcu (`micropython.cmake`)
   is left at CP's default pending phase 10's own RAM/polyphony call. Full
   detail, including the isolation methodology and the deterministic-PRNG
   test-harness technique needed for a byte-exact diff (`synthtools`
   reseeds oscillator phase via `random.randint()` by design), is in
   docs/upstream-diff.md. Full regression clean (tier 0-4 parity suite,
   LVGL smoke test) after the fix.
8. **Tier 6 outputs** *(8a-8e done; superseded design)*. The original plan
   here (a native SDL2-callback `AudioOut`) was replaced after actually
   reading `pydevices/lib/audiodev`: that package's whole design is
   push-based (`write()` + a mandatory per-tick `service()`) specifically
   *because* an audio-thread callback into Python is unsafe -- and that
   holds even in C once the callback would need to pull a `synthio` block
   graph, which allocates on the GC heap. The actual architecture: a new
   pure-Python pump, `audiodev.sample_out.AudioOut`
   (`pydevices/lib/audiodev/sample_out.py`), pulls PCM from any audiosample
   via `audiocore.get_buffer()`/`reset_buffer()` on a lookahead schedule and
   pushes it into any existing `audiodev` PCM transport (sdl2/win/wasm/i2s/
   emulated) -- giving CircuitPython's `play(sample, loop=)`/`stop()`/
   `pause()`/`resume()`/`playing` for free over every backend `audiodev`
   already supports, no output-device C code needed here at all. Full
   design writeup, board-by-board wiring, and rationale live in
   `pydevices/docs/audio.md` and `pydevices/lib/audiodev/README.md`
   ("`sample_out.py` -- `AudioOut`"), not here, since none of this touches
   the usermod tree.
   - **8a (done):** `AudioOut` written; every board's `audio_out` role
     rewired to return one (`board_configs/desktop`, the ESP32-P4, both
     M5Stack Tab5 variants, m5stack-cores3, t-embed, windisplay, jndisplay);
     `pygame_audio`/`web_audio` kept as raw-PCM-only transports (neither can
     back an `AudioOut` -- no usermod under CPython/PyScript) rather than
     deleted, since `pgdisplay`/`psdisplay` depend on them directly; full
     `pydevices` + `pydevices-examples` test suites green, including a new
     WAV-golden test (`tests/test_audio_playback_golden.py`) that renders a
     real `synthio`/`audiomixer` script through `AudioOut` on
     `bin/micropython`/`bin/circuitpython`/`micropython.exe` and diffs the
     output byte-for-byte -- passed on the first run across all three.
   - **8b (done):** `pydevices-examples/lib/examples/piano.py` migrated off
     the old `utils.audio.AudioEngine` onto a real `synthio.Synthesizer`
     (additive-wave voices, `synthio.Envelope`) through `AudioOut` -- the
     unix acceptance demo. `widgets_locker_kiosk.py` (still uses
     `AudioEngine` for simple UI blips) fixed to pull the raw transport via
     `audio_out.transport` now that the role returns an `AudioOut`; same fix
     applied to `audio_out_test.py`/`audio_in_playback_test.py`, the two
     raw-PCM portability smoke scripts. `utils/audio.py`/`AudioEngine`
     itself was *not* retired (a real remaining consumer), unlike the
     original phase 8 wording assumed.
   - **8c (done):** DSP parity suite (all tiers) plus
     `synthtools_acceptance.py` re-run under `bin/micropython.exe`,
     diffed against `bin/circuitpython` -- found and fixed a genuine,
     windows-only bug this way: `audiomp3`'s `stream_readable()` calls
     `MP_STREAM_POLL`, which mainline's own `extmod/vfs_posix_file.c` raises
     `NotImplementedError` from on `_WIN32` for a real file object, so
     `MP3Decoder` reading an `open()`ed file was completely broken on
     `micropython.exe`. Fixed with an `#ifdef _WIN32` fallback in
     `src/audiomp3/MP3Decoder.c` (see docs/upstream-diff.md, "Tier 5
     audiomp3: three Windows-only local fixes"). A mechanical `win_audio`
     `AudioOut` smoke (construct, play a `synthio.Note`, service, close, no
     hardware errors) also passed under `micropython.exe`; true by-ear
     verification is left for Brad, since this environment has no speakers.
   - **8d (mostly done):** Rebuilt `micropython.mjs`/`.wasm`
     (`build_interpreters.sh --only mp-wasm`); fixed five real
     emscripten-only portability bugs found along the way (an mp3dec.h
     platform-detection gap, two merged-header duplicate-typedef errors, a
     missing `M_PI`, dead-code/`-Wunused-function`, a missing
     `<errno.h>`/an implicit float promotion in `MP3Decoder.c` -- see
     docs/upstream-diff.md, "Phase 8d: WASM build fixes"). Ran the full DSP
     parity suite (tiers 0-5 + `synthtools_acceptance.py`) against the
     rebuilt interpreter via a headless Node driver
     (`loadMicroPython()`/`runPythonAsync`, no wasm-equivalent CLI exists)
     and diffed against `bin/circuitpython`: byte-for-byte identical except
     three documented, environment-only gaps in the ad hoc Node harness
     itself (no file staged for the mp3 fixture, a JS-wrapped traceback
     format, no `__file__` under `runPythonAsync`) -- none a DSP bug. Along
     the way, found and fixed a real, more consequential bug: tier 4's
     verbatim-kept `Distortion.soft_clip` union type-pun turned out to be
     architecture-dependent, not a stable quirk -- wrong on wasm32, and
     (by the same reasoning) most likely wrong on real 32-bit-ARM
     CircuitPython hardware too, which the earlier x86-64-only verification
     could never have caught. Fixed properly (`.u_bool`, not `.u_obj`); now
     produces byte-identical output across all three of this port's own
     targets (unix/windows/wasm) and a deliberate, documented divergence
     from the x86-64 CP oracle for that one field -- see docs/upstream-diff.md,
     "Distortion soft_clip: verbatim-kept union type-pun turned out to be
     architecture-dependent." Full regression clean on unix/windows/wasm
     after the fix (parity suite, LVGL smoke test on unix).
     Also extended `pydevices-examples/tools/wasm_browser/run_contract.py`
     with a new `check_audio_out` check: a real `synthio.Synthesizer`
     pressed/released and played through `audiodev.sample_out.AudioOut`
     over `wasm_audio.WasmPCMOutput`, driven by a fake manually-advanced
     clock (same technique as `audio_playback_golden_probe.py`) for
     determinism, asserting the resulting `AudioBufferSource` schedulings
     carry real, non-silent, non-clipped PCM -- not just that the DSP
     usermod imports. Passed cleanly (12 buffers, 3072 real PCM frames) on
     all three Playwright engines (chromium, firefox, webkit). 8d is done.
   - **8e (done):** ESP32-P4 (`esp32p4wifi6`) CMake decisions made while
     actually building the firmware: `CIRCUITPY_SYNTHIO_MAX_CHANNELS` raised
     from the header default of 2 (confirmed broken for real patches, see
     8d) to 8 for CMake/mcu ports generally -- a deliberately modest choice
     versus desktop/wasm's 14, since mcu RAM headroom varies far more than
     across this workspace's three host targets; audiomp3 deliberately still
     not wired for CMake ports (a concrete new blocker found: the parent
     workspace's `mp3/src/mp3dec.h`'s platform list has no RISC-V branch, and the P4 is
     RISC-V -- would need the same `MP3DEC_GENERIC` treatment as wasm, not
     worth building for a board with no MP3 demo). Firmware **built**
     (`./build_mp.sh --port esp32 --board ESP32_GENERIC_P4 --variant
     C6_WIFI`, no source changes needed beyond the CMake decisions above --
     the usermod compiled clean on RISC-V/ESP-IDF 5.5 on the first try) and
     measured: `micropython.bin` 3,254,688 bytes; the default factory
     partition (0x2f0000, ~3.09 MiB) was too small and the build's
     autosize-and-retry step resized it to 0x360000 (~3.375 MiB)
     automatically, landing at 8% partition headroom -- unremarkable given
     this board's flash size, but recorded here as the actual figure.
     **On-device verification done** once mpftp's new CLI (`docs/agent-guide.md`
     in the `mpftp` repo -- session model changed significantly since the
     "write-to-file" workaround this project used earlier; the standalone
     form is `./scripts/mpftp <cmd> -d COM4`, one full session per
     invocation) confirmed available: rebuilt the firmware (picking up the
     Distortion.c fix above), backed up `/wifi.py` and `/secrets.py` off
     the board first, flashed with `--erase` (required -- the new
     firmware's grown partition table didn't match the one already on the
     device, exactly as anticipated), restored both files, then pushed
     `audiodev` (including the new `sample_out.py`), `multimer`,
     `board_config.py`/`board_peripherals.py`, `boarddev.py`, and the
     driver files the board's own eager bring-up needs (`gt911`,
     `keypad_gpio`, `keys`, `displaydev`, ES8311/ES7210) directly over
     serial (`mpftp put`/`cp --verify`) -- deliberately not via `mip`,
     since that would have pulled the last-published (unmodified) copies
     from GitHub and overwritten the local fixes being verified. A probe
     script (real `synthio.Synthesizer`, note press/release through a real
     7-note arpeggio, `pause()`/`resume()`, hardware volume/mute via the
     ES8311 codec, `stop()`/`close()`) ran clean end to end on real
     hardware via `mpftp run --follow`, and ran identically clean a second
     time after `mpftp soft-reset` -- confirming both the audio pipeline
     itself and codec re-init survive a soft reset without stale state.
     Wi-Fi reconnected from the restored `secrets.py` and got a real DHCP
     lease, confirming the board is left in a working state. Phase 8 is
     now fully done, including on-device hardware verification.
9. **Tier 5 `audiomp3`** *(done)* — ported out of order (before phase 8,
   which is blocked on mpftp/hardware coordination), since nothing else
   depends on it and it needs no hardware. License checked first (see
   docs/upstream-diff.md, "Tier 5 audiomp3: license"): the Helix decoder
   core is RPSL 1.0/RCSL 1.0, not MIT, and this port carries it unmodified
   under those terms rather than relicensing it -- same as CircuitPython
   itself. Cloned `mp3` (upstream `adafruit/Adafruit_MP3`) as a parent
   workspace sibling dependency pinned to the exact commit its
   `circuitpython/lib/mp3` vendors, same treatment as `ulab`.
   `MP3Decoder` ported to `src/audiomp3/` (merged shared-bindings +
   shared-module, matching every other tier); needed a `background_callback`
   compat stub (`cp_compat/background_callback.h`) -- CircuitPython's own
   unix coverage build (this port's oracle) already collapses that whole
   API to a synchronous same-thread call, so there was nothing to actually
   port, only to make permanent. Oracle-diffed byte-for-byte against
   `bin/circuitpython` on a real MP3 file (the parent workspace's
   `mp3/examples/test.mp3`):
   full-track decode, RMS level, samples-decoded count, the
   `reset_buffer`-driven loop-to-start behavior (including a faithfully
   reproduced, non-obvious quirk -- a second pass decodes a different byte
   count than the first, confirmed identical on both interpreters, not a
   bug), a pre-allocated user buffer, stream-object input, `file`
   get/set, explicit `open()`, and error paths (text-mode file, malformed
   MP3 data). Needed two Windows-only local fixes with no unix impact: a
   patch to the vendored `mp3/src/assembly.h` in the parent workspace
   (its MSVC-only inline-asm branch also matched mingw-w64, which also
   defines `_WIN32`)
   and dropping `MP_WEAK` from this port's own `mp3_alloc`/`mp3_free`
   (mingw-w64's PE-COFF linker doesn't resolve a lone weak definition the
   way ELF does). Full regression clean (`build_interpreters.sh` for both
   unix and windows, LVGL smoke test, full tier 0-4 parity suite,
   `synthtools_acceptance.py`) after every change.
10. **Port matrix + footprint** *(done)*.
    Float/`mp_float_t` config already checked (phase 8d verification note):
    unix, windows, and wasm are all `MICROPY_FLOAT_IMPL_DOUBLE`, confirmed
    directly from each port's `mpconfigport.h`/variant headers, which is
    why DSP output stays byte-exact across all three. mcu build + measured
    flash/RAM cost: phase 8e's ESP32-P4 data point (`micropython.bin`
    3,254,688 bytes; app partition resized 0x2f0000 → 0x360000, 8% headroom)
    was a generous-flash board, not the smallest-target case this phase
    calls for. Closed that gap with a second, genuinely small/RAM-constrained
    build: `./build_mp.sh --port rp2 --board RPI_PICO` (RP2040 -- 2 MiB
    flash, 264 KiB RAM, no PSRAM, the most flash/RAM-starved board this
    workspace currently targets) -- built clean on the first try, same
    CMake glue as the P4 (no board-specific source changes needed).
    `firmware.bin`: 1,462,324 bytes against RPI_PICO's 2,097,152-byte flash
    -- 69.7% used, ~30.3% headroom -- with the *entire* workspace stack
    linked in (LVGL, ulab, pygraphics, displayif, audiodsp tiers
    0-4/6), not audio in isolation; tier 5 (audiomp3) is excluded on RP2040
    for the same reason it's excluded on the P4: not wired for any CMake
    port yet (see below), not a board-specific limitation -- RP2040's
    Cortex-M0+ is `__ARMEL__`, which the parent workspace's
    `mp3/src/mp3dec.h` already recognizes, so audiomp3 has no
    RP2040-specific blocker once someone
    does the CMake-port-detection work tier 8e deferred for RISC-V.
    Per-port enable flags, as they exist today:
    - **`CIRCUITPY_SYNTHIO_MAX_CHANNELS`** (max concurrent `Note`s per
      `Synthesizer`; header default 2, confirmed broken for real patches --
      see phase 7/8d): unix/windows set it to 14 (`micropython.mk`, matching
      CP's own unix `coverage` variant); every CMake/mcu port (esp32 *and*
      rp2, since `micropython.cmake` applies workspace-wide, not
      per-board) gets a flat 8 (`audiodsp/micropython.cmake`) --
      a deliberately modest, not-yet-per-board-tuned middle ground. A board
      with more headroom (P4 has PSRAM) or less can override via
      `target_compile_definitions`/`CFLAGS_EXTRA` per board; nothing in
      this workspace does that override yet, since 8 held up fine on both
      the P4 and the far-tighter RPI_PICO build.
    - **`CIRCUITPY_AUDIOMP3` / tier 5 wiring** *(now wired for CMake ports
      too)*: initially deferred here as "off for every CMake port pending
      RISC-V-style `MP3DEC_GENERIC` detection" -- closed that gap rather
      than leaving it open, since it turned out narrower than expected:
      mp3dec.h's closed list has no Xtensa branch either, so *every* esp32
      target (Xtensa: esp32/s2/s3; RISC-V: c2/c3/c5/c6/p4) needs the same
      escape hatch, while rp2 (ARM, native match) needs nothing.
      `audiodsp/micropython.cmake` now wires tier 5 the same way
      `micropython.mk` does (sources, include dir, the `mp3_alloc`
      allocator `-include`/`-fwrapv` on `buffers.c`), gated by ESP-IDF's
      own `CONFIG_IDF_TARGET_ARCH_RISCV`/`_ARCH_XTENSA` sdkconfig variables
      (the same ones `esp32_common.cmake` itself branches on) rather than
      re-deriving "which CMake port is this" from scratch. One genuine new
      wrinkle found while wiring it, not anticipated at the P4 8e writeup:
      `target_compile_definitions(usermod_mpaudio INTERFACE
      MP3DEC_GENERIC)` reaches the real per-file compile fine (ninja
      resolves the INTERFACE chain), but ESP-IDF's QSTR-extraction
      preprocessing pass (`py/mkrules.cmake`'s `qstr.i.last` rule) builds
      its own flag list via a raw `get_target_property(MICROPY_TARGET
      COMPILE_DEFINITIONS)` that does *not* walk linked INTERFACE
      libraries -- confirmed by diffing that pass's actual `-E` invocation
      against the real compile rule's `DEFINES` (present in the latter,
      absent in the former), and it hit mp3dec.h's `#error` directly since
      QSTR scanning still has to fully preprocess every source.
      `py/py.cmake`'s own `micropy_gather_target_properties()` exists to
      bridge exactly this INTERFACE-library gap, but `esp32_common.cmake`
      only calls it over registered ESP-IDF components -- "usermod" isn't
      one, it's a plain target inside the main component, so that loop
      never reaches it. Fixed portably (not an esp32-only patch) by
      appending directly to `MICROPY_CPP_DEF_EXTRA`, the same accumulator
      both that gather-loop and `mkrules.cmake`'s own qstr flag-building
      read from -- a normal (non-cache) CMake variable write, valid because
      usermod.cmake (and our aggregator beneath it) is `include()`d, not
      `add_subdirectory()`d, into the port's own variable scope.
      Verified end to end: rebuilt both mcu data points from scratch.
      ESP32-P4 (`--port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI`,
      the `MP3DEC_GENERIC`/RISC-V branch): clean build,
      `micropython.bin` grew 3,254,688 → 3,293,648 bytes against the same
      0x360000 partition, headroom 8% → 7%. RPI_PICO (the native-ARM
      branch, no `MP3DEC_GENERIC` needed): clean build, `firmware.bin` grew
      1,462,324 → 1,497,556 bytes against 2,097,152, headroom ~30.3% →
      ~28.6%. Both real, fresh builds, not just "no compile errors" --
      confirms the fix holds on both branches of the arch split, and
      audiomp3 is no longer a per-board-flash decision CircuitPython itself
      still has to make on some targets (`espressif`, `atmel-samd` both
      default `CIRCUITPY_AUDIOMP3` off there) that this port simply can't
      offer -- it can now, and per-board on/off is a normal
      `target_compile_definitions` override away if a future board needs
      the flash back.
    - No board in this workspace currently needs a *narrower* audio build
      (e.g. audiocore-only, no synthio/mixer/effects) to fit -- both mcu
      data points (P4, RPI_PICO) fit the full tier 0-4/6 stack with double-
      digit headroom. If a future board doesn't, the module-per-tier `.c`
      file layout in `micropython.cmake`'s `target_sources()` list is
      already the seam to split on (drop a tier's `target_sources` +
      `mpaudio_modules.c` registration entries; the Python-level `import`
      then just fails on that module, matching real CircuitPython's own
      `CIRCUITPY_AUDIOFOO=0` behavior).
11. **Spin-off.** Split into its own PyDevices repo, symlink back into
    the parent workspace like `pygraphics`/`displayif`, re-ignore the
    path here.

## Testing strategy (recap)

- **Oracle diffing is the core:** every DSP tier lands with scripts under
  `tests/parity/` that run unchanged on `bin/circuitpython` and the MP
  build, rendering PCM to stdout/file; the runner asserts byte-equality
  (or bounded error where float-width differences make byte-exactness
  impossible — record any such case in `upstream-diff.md`).
- Mainline-MP-only concerns (protocol slot behavior, soft reset, output
  lifecycle) get ordinary unit tests in `tests/`.
- CI-friendly throughout: no audio hardware needed until phase 7, and even
  then WAV-capture paths keep it scriptable.

## Open questions

- **`lib/mp3` license** — CP vendors an MP3 decoder; confirm its license
  allows carrying into this repo before tier 5.
- **`mp_float_t` width.** CP unix builds double-precision; MP mcu configs
  are often single-float. Decide early (phase 4) whether parity tests
  demand double on all targets or tolerate bounded drift on single-float
  mcu builds.
- **ulab pin drift.** CP 10.2.1 pins ulab 6.5.2; upstream moves. Pin ours
  to CP's revision and revisit only when the CP checkout moves.
- **Frozen-vs-mip delivery of `synthtools`** for phase 6 (vendored copy in
  tests vs `mip.install` at test time).
