# audiorender

Renders a whole composition offline through audiodsp's own DSP: tracks,
tempo map, notes, automation and sections in, a mixed stereo master and a
level report out. This is the tier above `audioinstruments` and
`audioeffects`, and unlike them it is unapologetically a desktop library:
numpy throughout, the whole song held in memory. What it renders is the
same audio a board would play, sample for sample - the DSP underneath is
the same C.

## Installation

`audiorender` ships inside the `pydevices-audiodsp` distribution itself,
not as a separate package, and is never frozen into firmware or meant for
a board. It needs `numpy`, which the distribution declares as the `render`
extra rather than as a dependency — the core modules run on boards and in
wasm, where numpy has no place — so ask for the extra:

```sh
python -m pip install --index-url https://test.pypi.org/simple/ \
    --extra-index-url https://pypi.org/simple/ "pydevices-audiodsp[render]"
```

The second index is not optional: TestPyPI carries no numpy anyone can
use — a 2.4.0.dev0 pre-release pip will not pick, and a 2015 1.9.3 sdist
that cannot build on a supported Python — so without PyPI in the search
path the extra fails to install. Installing `pydevices-audiodsp` without
`[render]` still gets you every core module; `import audiorender` is then
the one thing that fails, on numpy.

The quick start below also renders through `audioinstruments` voices; that
package lives in
[audiocomponents](https://github.com/PyDevices/audiocomponents#installation)
now and installs from there.

## Quick start

```python
import audioinstruments, audiorender

def voice_for(track, clock):
    return audiorender.Voice(audioinstruments.create(
        track["instrument"], composition.SAMPLE_RATE, transport=clock))

master = audiorender.render(composition, voice_for, out=print)
audiorender.report(master)
audiorender.write_wav("out.wav", master.data, master.sample_rate)
```

## The composition contract

A composition is any module or object carrying:

| Name | Meaning |
|---|---|
| `TITLE` | what to call the piece |
| `SAMPLE_RATE` | frames per second |
| `MASTER_GAIN_DB` | applied to the sum of every track |
| `TEMPO_MAP` | `[(beat, bpm[, ts_num, ts_den]), ...]`, piecewise constant |
| `TOTAL_BEATS` | where the piece ends |
| `SONG_SECONDS` / `RENDER_SECONDS` | the same point in seconds, and how far to render past it |
| `SECTIONS` | `[(name, first beat, last beat), ...]` |
| `TRACKS` | see below |
| `ACTIVE_LIMIT` | optional cap on simultaneous tracks |
| `beats_to_seconds(beat)` | the tempo map, applied |
| `macro_value(track, index, beat)` | a macro at a moment, normalized 0.0-1.0 |
| `track_gain(track, beat)` | the fader at a moment, as amplitude |
| `active_track_count(beat)` | how many tracks sound at that beat |

A track is a dict with `name`, `script`, `pan` (-1.0 to 1.0), `notes` as
`[(start beat, duration, pitch, velocity), ...]`, `macros`, `macro_env`,
and optionally `programs` and `effects`.

`TempoMap` reimplements `beats_to_seconds` so a render can also go the
other way - `beat_at_sample`, `bpm_at_beat`, `timesig_at_beat` - and it is
checked against the composition's own function rather than trusted.

## Voices

Loading a track's sound is the caller's job. A plug-in host has its own
script loader and its own reasons to keep it, so `render()` asks for a
`voice_for(track, clock)` and only requires what it comes back with to
answer two calls:

```python
deliver(event, sample_position)
pull_frames(frames) -> interleaved little-endian stereo int16
```

`Voice` is that protocol over an `audioinstruments` instrument, which is
what you want unless you are that host. `Puller` is the frame-buffering
half on its own, for any audiodsp node.

`Clock` is what tempo-locked instruments read. Build instruments with it as
their `transport` and hand the same object to `render()`, which moves it to
the head of every block.

## Two rules worth knowing

**Events at the same sample happen in one order**: macro values, then a
program change (which replaces all of them), then note-offs, then
note-ons. With the program change ahead of the opening macros the patch is
applied and immediately overwritten, and the render disagrees with what a
host would play by several dB - silently.

**A macro the composition never mentions resolves to the instrument's own
patch**, via `patch_for`, not to 0.5. Half a range is not "off" and not
what the instrument's author meant; pass the patch or accept that every
unset macro sits in the middle.

## The report

`report(master)` prints per-track section levels, the master peak, a
section profile, and the busiest beat in the piece - then returns whether
the render is usable, meaning it neither clipped nor exceeded
`ACTIVE_LIMIT`. The `hp150` column measures only energy above 150 Hz, so
sub bass does not dominate the numbers the way it does not dominate the
ear.

These numbers are the contract a render is checked against: a bounce
through a real host is compared to them section by section.
