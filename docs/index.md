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
- [Render on several threads](guides/use-threads.md), including free-threaded Python.

## Reference

- [The modules](reference/modules.md): what ships, and where each is documented.
- [Filters and effects](reference/filters-and-effects.md): `audiobiquad`, `audiodynamics`, `audioecho`, `audioshaper`, `audioladder`, `audioverb`.
- [Sources and synthesis](reference/sources-and-synthesis.md): `audiooscillator`, `audiomodal`, `audiograph`.
- [audiopump](reference/audiopump.md): status, builds, sources.
- [Vorbis](reference/vorbis.md): the C interface, memory and tests.

## How it works

- [How audiodsp works](how-it-works/index.md): the pull protocol, the three targets, the repository map.
- [Additions beyond CircuitPython](how-it-works/additions.md)
- [Sound stability](how-it-works/sound-stability.md)
- [The correctness standard](how-it-works/correctness-standard.md)
- [Keeping in step with upstream](how-it-works/upstream-sync.md)
- [Differences from upstream, with measurements](how-it-works/upstream-diff.md)
- [Upstream reports](upstream-reports/README.md): the fixes sent to CircuitPython.
- [The graph runner](how-it-works/audiograph.md)
- [Writing a pump port](how-it-works/pump-ports.md)
- [Wheels and platforms](how-it-works/wheels-and-platforms.md)

## Changelog

- [Changelog](changelog.md): what changed in each release.

[Roadmap](../ROADMAP.md) · [Contributing](../CONTRIBUTING.md)
