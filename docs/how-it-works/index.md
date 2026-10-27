# How audiodsp works

`audiodsp` brings CircuitPython's audio-programming surface to MicroPython and
CPython. It supplies familiar top-level modules—`audiocore`, `synthio`,
`audiomixer`, `audiofilters`, and related effect modules—so an audio graph can
keep the same import names across those runtimes.

The repository is an audio *core*, not a complete sound application. It
provides sources, processors, mixing, and the portable audio pump; a board or
desktop integration provides the physical output device. Instruments and
high-level effects belong in the separate
[audiocomponents](https://github.com/PyDevices/audiocomponents) repository.

## Start by choosing a runtime

On CPython the wheel gives you the modules as top-level imports, just as
they are on a board. On MicroPython, audiodsp is compiled into firmware
through [`manifest.py`](../../manifest.py). [Install](../get-started/install.md)
has the commands for each, and [Your first program](../get-started/first-program.md)
renders a block of audio. A host can pull the graph for offline rendering,
give it to a mixer or pump, or attach it to a platform-specific audio output.
`audiomp3` is firmware-only.

## The mental model

```text
Python graph
  |-- source: RawSample, WaveFile, Synthesizer, MidiTrack, ...
  |-- processors: filters, delays, dynamics, routing, ...
  `-- mixer or audiopump
          |
          `-- platform driver / host pull loop / offline renderer

shared C DSP and sample protocol
  |-- MicroPython user modules
  |-- CPython _audiodsp extension and Python compatibility shims
  `-- additive CircuitPython extension path
```

The audiosample pull protocol is the spine of the design: an upstream source
returns audio blocks when a downstream consumer asks. Nodes therefore compose
without Python copying every sample. `audiopump` owns the portable live-audio
loop mechanics; a particular port supplies its clock, threading, and sink.
For a new pump port, [the pump-port guide](pump-ports.md) is authoritative.

## Repository map

| Path | Purpose |
|---|---|
| `src/audiocore/`, `src/synthio/`, `src/audiomixer/` | Core CircuitPython-compatible source, synthesis, and mixing modules. |
| `src/audio*/` | Effects, DSP, routing, and audio utility modules compiled into native targets. |
| `src/audiopump/` | Portable pump loop, rings, events, taps, and lifecycle guards. |
| `src/shared/` | DSP and sample-protocol C shared by the MicroPython and CPython targets. |
| `src/cp_compat/` | Small CircuitPython compatibility shims needed by the MicroPython port. |
| `src/cpython/` | `_audiodsp` extension plus top-level Python wrappers for CPython. |
| `lib/audiodsp_util/` | Cross-runtime `float32()` helpers for settings derived in Python. |
| `lib/audiorender/` | Desktop-only, NumPy-based offline composition renderer. |
| `micropython.mk`, `micropython.cmake` | User-module build glue for Make and CMake MicroPython ports. |
| `tests/` | CPython fixtures, in-repository parity gates, and parity tools that need sibling checkouts. |
| `docs/` | Guides, reference, and these architecture pages. |

## One graph, three target shapes

On MicroPython, `micropython.mk` or `micropython.cmake` compiles the module
sources into the firmware. On CPython, `setup.py` builds the same shared C
kernel into `_audiodsp`; the Python files in `src/cpython/` present the
CircuitPython-compatible public API. This is why `import synthio` is portable
without importing a package named `audiodsp` first.

CircuitPython is an oracle and an extension target, not a working tree to
edit. audiodsp-owned modules build into CircuitPython as a user C module
(`circuitpython.mk`), but stock behavior is not changed there. Read
[the correctness standard](correctness-standard.md) before any parity- or
CircuitPython-adjacent work.

`audiorender` deliberately stays outside firmware: it holds a full composition
in memory and requires NumPy. [Install](../get-started/install.md#cpython-311-or-later)
shows its optional extra.

## Important boundaries

- `audioinstruments` and `audioeffects` no longer ship from this repository.
  Install and change them in `audiocomponents`.
- The `mp3` native dependency is a cloned, pinned dependency; it is not
  vendored source to edit here.
- `audiodsp_util.float32()` matters when Python computes a setting that must
  have board-equivalent single-precision behavior on a desktop. It is not a
  general-purpose numeric library.
- The project distinguishes CircuitPython-compatible modules from audiodsp's
  own DSP additions such as dynamics, routing, convolution, ladder filtering,
  and modal synthesis. The latter have target-agreement and numeric-trait
  checks rather than an upstream oracle.

## Testing and contributing

Most deep parity checks need the sibling repositories checked out beside
this one, built interpreters, and the fixed CircuitPython oracle. They are
not a standalone contributor prerequisite. The repository's CI covers CPython
fixture tests and committed in-repository parity gates; `python -m flake8` is
the local lint gate. [CONTRIBUTING.md](../../CONTRIBUTING.md) has the rules:
don't modify the CircuitPython oracle, don't casually update parity goldens,
and don't move the dependency pins.

Start with a documentation correction, a focused CPython fixture, or a
well-bounded module change; then read [the upstream-sync record](upstream-sync.md)
for the module's intended behavior and known deliberate differences.

For a first tour of the implementation, follow one simple source through
`audiocore`'s sample interface, a `synthio.Synthesizer`, and `audiomixer` or
`audiopump`. That path explains the common contract behind the larger effect
catalogue better than reading the module directories alphabetically.

## Names

audiodsp was named `audioif` earlier; that name now belongs to the audio
hardware layer. The modules you import did not change. The helper package is
`audiodsp_util`, and the distribution on PyPI is `pydevices-audiodsp`.

## How it is checked

The committed synthesis, mixer, MIDI, streaming and effects fixtures match
CircuitPython 11.0.0-alpha.2 PCM byte-for-byte, built at the same voice
ceiling this port ships (the pin is in `CIRCUITPYTHON_ORACLE`). Import and API
smoke success is not used as a substitute for those comparisons. The
playback-facing pull protocol (`audiocore.get_buffer` and `reset_buffer`) is
consumed by `pydevices`' `lib/audiodev` package (`AudioOut` in
`sample_out.py`), which pumps any audiosample into any of `audiodev`'s
push-PCM transports.

## Pages in this section

- [Additions beyond CircuitPython](additions.md): why each added module exists.
- [Sound stability](sound-stability.md): what is stable across releases, and what isn't.
- [The correctness standard](correctness-standard.md): how each module is held to a reference.
- [Keeping in step with upstream](upstream-sync.md): the resync procedure, and every deliberate difference.
- [Differences from upstream, with measurements](upstream-diff.md): the reasons behind each one.
- [Upstream reports](../upstream-reports/README.md): the fixes sent to CircuitPython.
- [The graph runner](audiograph.md): how `audiograph.Graph` renders a patch.
- [Writing a pump port](pump-ports.md): how a platform plugs in a clock, a thread and a sink.
- [Wheels and platforms](wheels-and-platforms.md): what the release builds, and what it skips.
