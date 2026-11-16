# The modules

audiodsp is CircuitPython's audio system for MicroPython and CPython:
`audiocore`, `audiomixer`, `synthio` (including `MidiTrack`), the effects
modules (`audiofilters`, `audiodelays`, `audiofreeverb`, `audiospeed`),
`audiomp3`, and CircuitPython's `play()`, `stop()`, `pause()` and `resume()`
output semantics. Import names stay `audiocore`, `synthio` and the rest,
matching CircuitPython, so source runs unchanged; the few behaviours that
differ are in
[Where audiodsp differs from CircuitPython](../how-it-works/upstream-diff.md).

It adds modules CircuitPython doesn't have ([why each exists](../how-it-works/additions.md)). Each has a page here or a guide:

| Module | What it does | Where |
|---|---|---|
| `audiobiquad` | Biquad and all-pass filters whose tails reach exact zero | [guide](../guides/filter-a-sound.md), [reference](filters-and-effects.md#audiobiquad) |
| `audiodynamics` | Compression, limiting, expansion, gating, transient shaping | [reference](filters-and-effects.md#audiodynamics) |
| `audioecho` | A delay with a filter, a soft-clip and a cross-feed in its feedback loop | [reference](filters-and-effects.md#audioecho) |
| `audioroute` | `Splitter` fans a stream out; `MidSide` scales stereo width; `Port` is a re-pointable wire | [MidSide](filters-and-effects.md#audioroutemidside), [Port guide](../guides/change-without-a-click.md#swap-one-part-of-the-graph-through-a-port) |
| `audioshaper` | A waveshaper whose curve is data, and an exact-ratio sample-and-hold | [reference](filters-and-effects.md#audioshaper) |
| `audioladder` | A transistor ladder filter | [reference](filters-and-effects.md#audioladder) |
| `audioverb` | A reverberation tank whose line lengths and taps come from Python | [reference](filters-and-effects.md#audioverb) |
| `audiomath` | Multiply one stream by another, divide one down in frequency, and `cast_into` | [reasons](../how-it-works/additions.md) |
| `audioconvolve` | Apply an impulse response by partitioned FFT | [reasons](../how-it-works/additions.md) |
| `audiooscillator` | A band-limited oscillator read sample by sample | [reference](sources-and-synthesis.md#audiooscillator) |
| `audiomodal` | A bank of resonators | [reference](sources-and-synthesis.md#audiomodal) |
| `audiograph` | A whole patch rendered by one C call per block | [reference](sources-and-synthesis.md#audiograph), [how it works](../how-it-works/audiograph.md) |
| `audiosampler` | Recorded notes played at any pitch, looped, under an envelope | [guide](../guides/play-recorded-notes.md) |
| `audiometer` | Band levels for a spectrum meter | [guide](../guides/meter-levels.md) |
| `audiofeatures` | Speech features and voice activity for a wake word model | [guide](../guides/speech-features.md) |
| `audiovorbis` | Ogg Vorbis files | [guide](../guides/decode-vorbis.md), [C interface](vorbis.md) |
| `audiopump` | The audio pull, off the interpreter thread | [guide](../guides/run-the-audio-pump.md), [reference](audiopump.md) |

## Beside the core

Two pure-Python modules sit beside the CircuitPython-compatible core:

- **[`lib/audiodsp_util/`](https://github.com/PyDevices/audiodsp/tree/main/lib/audiodsp_util)** — `float32(value)` and `float32_bits(value)`,
  `struct` and nothing else. A Python float is the interpreter's own width:
  a double here, a **single** on every board. So a setting written in Python
  — `node.mix = 0.35` — is two different numbers, and the node renders
  different bytes on a board than on a desktop before its kernel is reached.
  Put a derived setting through `float32` and both targets hold the number
  the board would have held. See
  [docs/correctness-standard.md](../how-it-works/correctness-standard.md).
- **[`lib/audiorender/`](https://github.com/PyDevices/audiodsp/tree/main/lib/audiorender)** — renders a whole composition offline: tracks,
  tempo map, notes and automation in, a mixed stereo master and a level
  report out. This is the one part of the repository written for a desktop
  rather than a board — numpy throughout, a whole song in memory — so it
  ships in the wheel and is never frozen into firmware. It has its own
  README.

Instruments and effects built on these modules are in their own libraries,
[audioinstruments](https://github.com/PyDevices/audioinstruments) and
[audioeffects](https://github.com/PyDevices/audioeffects), which also define
the component contract they share. `audiorender` drives any component that
speaks that contract and doesn't need either library installed. What stays the
same across releases, for audiodsp and for them, is in
[Sound stability](../how-it-works/sound-stability.md).
