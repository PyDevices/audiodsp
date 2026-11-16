# audiodsp documentation

audiodsp is the OmniPython audio engine: CircuitPython's audio modules
(`audiocore`, `synthio`, `audiomixer` and the effects) on MicroPython and
CPython with the same import names, plus filters, a reverb, a sampler, a
Vorbis decoder, a level meter and an audio pump that CircuitPython doesn't
have. Start with install, then your first program.

## Get started

- [Install](get-started/install.md): CPython wheel, MicroPython manifest, CircuitPython user module.
- [Your first program](get-started/first-program.md): measure a filter, with its output.

## Guides

- [Play computed audio](guides/play-computed-audio.md) with `audiopump.Ring`.
- [Run audio off the interpreter thread](guides/run-the-audio-pump.md) with `audiopump`, or on several threads of your own.
- [Change what plays without a click](guides/change-without-a-click.md) with `fade=` or `audioroute.Port`.
- [Decode Ogg Vorbis](guides/decode-vorbis.md).
- [Filter a sound](guides/filter-a-sound.md) with `audiobiquad`.
- [Play recorded notes](guides/play-recorded-notes.md) with `audiosampler`.
- [Meter levels](guides/meter-levels.md) for a spectrum display.
- [Speech features](guides/speech-features.md) for a wake word model.

## Reference

- [The modules](reference/modules.md): what ships, and where each is documented.
- [Sources and synthesis](reference/sources-and-synthesis.md): `audiocore`, `audiomp3`, `synthio`, `audiooscillator`, `audiomodal`, `audiosampler`, `audiograph`.
- [Filters and effects](reference/filters-and-effects.md): `audiobiquad`, `audiodynamics`, `audioecho`, `audioshaper`, `audioladder`, `audioverb`, `audiofilters`, `audiodelays`, `audiofreeverb`, `audiospeed`, `audioconvolve`.
- [Mixing and routing](reference/mixing-and-routing.md): `audiomixer`, `audioroute`.
- [Analysis and utilities](reference/analysis-and-utilities.md): `audiometer`, `audiofeatures`, `audiomath`.
- [audiopump](reference/audiopump.md): status, builds, sources.
- [Vorbis](reference/vorbis.md): `audiovorbis`, the C interface, memory and tests.

## How it works

- [How audiodsp works](how-it-works/index.md): the pull protocol, the three targets, the repository map.
- [Additions beyond CircuitPython](how-it-works/additions.md)
- [Where audiodsp differs from CircuitPython](how-it-works/upstream-diff.md)
- [Sound stability](how-it-works/sound-stability.md)
- [The correctness standard](how-it-works/correctness-standard.md)
- [The graph runner](how-it-works/audiograph.md)
- [The drift resampler](how-it-works/drift-resampler.md)
- [Writing a pump port](how-it-works/pump-ports.md)
- [Wheels and platforms](how-it-works/wheels-and-platforms.md)

## Changelog

- [Changelog](changelog.md): what changed in each release.

[Roadmap](https://github.com/PyDevices/audiodsp/blob/main/ROADMAP.md) · [Contributing](https://github.com/PyDevices/audiodsp/blob/main/CONTRIBUTING.md)

```{toctree}
:hidden:

changelog
get-started/first-program
get-started/install
guides/change-without-a-click
guides/decode-vorbis
guides/filter-a-sound
guides/meter-levels
guides/play-computed-audio
guides/play-recorded-notes
guides/run-the-audio-pump
guides/speech-features
how-it-works/additions
how-it-works/audiograph
how-it-works/correctness-standard
how-it-works/drift-resampler
how-it-works/index
how-it-works/pump-ports
how-it-works/sound-stability
how-it-works/upstream-diff
how-it-works/wheels-and-platforms
reference/analysis-and-utilities
reference/audiopump
reference/filters-and-effects
reference/mixing-and-routing
reference/modules
reference/sources-and-synthesis
reference/vorbis
```
