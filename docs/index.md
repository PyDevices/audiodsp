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
- [Run audio off the interpreter thread](guides/run-the-audio-pump.md) with `audiopump`.
- [Change what plays without a click](guides/change-without-a-click.md) with `fade=`.
- [Change what plays without rewiring](guides/swap-what-plays.md) with `audioroute.Port`.
- [Decode Ogg Vorbis](guides/decode-vorbis.md).
- [Filter a sound](guides/filter-a-sound.md) with `audiobiquad`.
- [Play recorded notes](guides/play-recorded-notes.md) with `audiosampler`.
- [Meter levels](guides/meter-levels.md) for a spectrum display.
- [Speech features](guides/speech-features.md) for a wake word model.
- [Render on several threads](guides/use-threads.md), including free-threaded Python.

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
- [Sound stability](how-it-works/sound-stability.md)
- [The correctness standard](how-it-works/correctness-standard.md)
- [Running the parity gates](how-it-works/parity-gates.md)
- [Keeping in step with upstream](how-it-works/upstream-sync.md)
- [Differences from upstream, with measurements](how-it-works/upstream-diff.md)
- [Upstream reports](upstream-reports/README.md): the fixes sent to CircuitPython.
- [The graph runner](how-it-works/audiograph.md)
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
guides/swap-what-plays
guides/use-threads
how-it-works/additions
how-it-works/audiograph
how-it-works/correctness-standard
how-it-works/drift-resampler
how-it-works/index
how-it-works/parity-gates
how-it-works/pump-ports
how-it-works/sound-stability
how-it-works/upstream-diff
how-it-works/upstream-sync
how-it-works/wheels-and-platforms
porting-plan
reference/analysis-and-utilities
reference/audiopump
reference/filters-and-effects
reference/mixing-and-routing
reference/modules
reference/sources-and-synthesis
reference/vorbis
upstream-reports/README
upstream-reports/biquad-band-edges
upstream-reports/biquad-reset
upstream-reports/dds-oscillator-off-by-one
upstream-reports/distortion-overdrive-drive
upstream-reports/distortion-soft-clip-union
upstream-reports/peaking-eq-sign
upstream-reports/prs/biquad-reset/PR
upstream-reports/prs/dds-oscillator-off-by-one/PR
upstream-reports/prs/distortion-soft-clip-union/PR
upstream-reports/prs/flanger-int32-overflow/PR
upstream-reports/prs/peaking-eq-sign/PR
upstream-reports/prs/speedchanger-phase-carry/PR
upstream-reports/prs/speedchanger-rate-rounding/PR
upstream-reports/prs/synthio-note-repress-zero-level/PR
upstream-reports/prs/synthio-note-repress-zero-level/review-reply-relic-se
upstream-reports/speedchanger-phase-carry
upstream-reports/speedchanger-rate-rounding
```
