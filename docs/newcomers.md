# Newcomer's guide to audiodsp

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

For CPython 3.11 or newer, install the published wheel from TestPyPI:

```bash
python -m pip install --index-url https://test.pypi.org/simple/ \
    pydevices-audiodsp
```

The installed modules are top-level imports, just as they are on a board:

```python
from array import array

import audiocore
import synthio

synth = synthio.Synthesizer(sample_rate=8_000)
synth.press(69)
status, block = audiocore.get_buffer(synth)
```

That example renders a block from a source; it does not select a speaker. A
host can pull the graph for offline rendering, give it to a mixer or pump, or
attach it to a platform-specific audio output. `audiomp3` is firmware-only.

For MicroPython, audiodsp is compiled into firmware through
[`manifest.py`](../manifest.py), with the pinned `ulab` and `mp3` dependencies
retrieved by `scripts/fetch_deps.sh`. The root [README](../README.md#installation)
has the supported CMake and Make build paths. It explains when a custom
manifest must include the port's own frozen modules as well.

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
| `tests/` | CPython fixtures, in-repository parity gates, and workspace-level parity tools. |
| `docs/` | Detailed architecture, correctness, porting, wheel, and pump-port documentation. |

## One graph, three target shapes

On MicroPython, `micropython.mk` or `micropython.cmake` compiles the module
sources into the firmware. On CPython, `setup.py` builds the same shared C
kernel into `_audiodsp`; the Python files in `src/cpython/` present the
CircuitPython-compatible public API. This is why `import synthio` is portable
without importing a package named `audiodsp` first.

CircuitPython is an oracle and an extension target, not a working tree to
edit. `apply_cp_patches.sh` can add audiodsp-owned modules to a CircuitPython
tree, but stock behavior is not changed there. Read
[the correctness standard](correctness-standard.md) before any parity- or
CircuitPython-adjacent work.

`audiorender` deliberately stays outside firmware: it holds a full composition
in memory and requires NumPy. Install its optional extra on CPython when you
need it:

```bash
python -m pip install --index-url https://test.pypi.org/simple/ \
    --extra-index-url https://pypi.org/simple/ "pydevices-audiodsp[render]"
```

## Important boundaries

- `audioinstruments` and `audioeffects` no longer ship from this repository.
  Install and change them in `audiocomponents`.
- The `ulab` and `mp3` native dependencies are cloned, pinned dependencies;
  they are not vendored source to edit here.
- `audiodsp_util.float32()` matters when Python computes a setting that must
  have board-equivalent single-precision behavior on a desktop. It is not a
  general-purpose numeric library.
- The project distinguishes CircuitPython-compatible modules from audiodsp's
  own DSP additions such as dynamics, routing, convolution, ladder filtering,
  and modal synthesis. The latter have target-agreement and numeric-trait
  checks rather than an upstream oracle.

## Testing and contributing

Most deep parity checks need the larger PyDevices workspace, built
interpreters, and the fixed CircuitPython oracle. They are intentionally not a
standalone contributor prerequisite. The repository's CI covers CPython
fixture tests and committed in-repository parity gates; `python -m flake8` is
the local lint gate.

Before changing source, read [AGENTS.md](../AGENTS.md). In particular, do not
modify the CircuitPython oracle, casually update parity goldens, or move the
dependency pins. Start with a documentation correction, a focused CPython
fixture, or a well-bounded module change; then consult
[the porting plan](porting-plan.md) and
[the upstream-diff record](upstream-diff.md) for the module's intended
behavior and known deliberate deviations.

For a first tour of the implementation, follow one simple source through
`audiocore`'s sample interface, a `synthio.Synthesizer`, and `audiomixer` or
`audiopump`. That path explains the common contract behind the larger effect
catalogue better than reading the module directories alphabetically.
