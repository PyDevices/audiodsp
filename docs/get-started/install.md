# Install

Pick the channel that matches where your code runs: a desktop with CPython,
a MicroPython build, or a CircuitPython build. Once it's installed,
[your first program](first-program.md) renders a block of audio.

## CPython 3.11 or later

**CPython 3.11+** installs from PyPI:

```sh

The distribution on PyPI is `pydevices-audiodsp`.

```

This gets you `audiocore`, `synthio`, `audiomixer`, `audiofilters`,
`audiodelays`, `audiofreeverb`, `audiospeed`, `audiodynamics`, `audioroute`,
`audiomath`, `audioecho`, `audioshaper`, `audioladder`, `audiooscillator`,
`audioconvolve`, `audiobiquad`, `audioverb`, `audiomodal`, `audiosampler`, `audiometer`,
`audiofeatures`, `audiopump`, `audiovorbis`, `audiograph`, and the `audiorender` package.
`audiomp3` remains firmware-only. The distribution declares no *required*
runtime dependencies and does not itself publish an `audiodsp` import; its
version is the `VERSION` file, which is also what `_audiodsp.__version__`
reports.


`audiorender` is the exception, and it is opt-in: it is numpy throughout,
so numpy comes with the `render` extra rather than with the wheel, keeping
the core dependency-free for boards and wasm:

```sh
python -m pip install "pydevices-audiodsp[render]"
```


The instrument and effect libraries, `audioinstruments` and `audioeffects`,
are not part of this distribution. They live in the
[audiocomponents](https://github.com/PyDevices/audiocomponents) repository
as their own packages, each depending on `pydevices-audiodsp`; see that
repository for how to install them.

To render on several threads, see [Render on several threads](../guides/use-threads.md).

## MicroPython 1.29 or later

**MicroPython 1.29 or later** takes this repository with one line in the
manifest your build already uses. Clone it anywhere, fetch its two pinned
native dependencies, and include its [manifest.py](../../manifest.py):

```sh
git clone https://github.com/PyDevices/audiodsp ~/build/audiodsp
~/build/audiodsp/scripts/fetch_deps.sh
```

```python
include("/home/you/build/audiodsp/manifest.py")
```

On unix the manifest to edit is `ports/unix/variants/standard/manifest.py`; on
esp32 and rp2 it is usually `ports/<port>/boards/manifest.py`, unless your
board brings its own. Then build the way you always do. The include brings in
the audiodsp C modules (named with `c_module()`); it freezes no Python and
brings no `ulab`, which audiodsp doesn't use: a build that wants `ulab` names
it itself. `fetch_deps.sh` puts the `mp3` decoder under `.deps/`, pinned by
[DEPENDENCIES.lock](../../DEPENDENCIES.lock); the mp3 glue looks there first and
falls back to a sibling checkout beside this repo. On a unix build, `synthio`, `audiomp3`
and `audiopump` all import.


If you would rather not edit the MicroPython tree, write a manifest of your
own and pass it as `FROZEN_MANIFEST=`. That replaces the port's default, so
include the default too (`include("$(PORT_DIR)/variants/standard/manifest.py")`
on unix, `include("$(PORT_DIR)/boards/manifest.py")` on esp32) or you lose
`asyncio` and the port's other frozen modules.

To build without `mp3`, skip the fetch and set `AUDIODSP_OPTIONAL_DEPS=1` on
the **build** step — `micropython.mk`/`micropython.cmake` read it, not
`fetch_deps.sh`, and on CMake ports it is an *environment* variable, not a
`-D` cache entry.


**Older than 1.29?** Manifests there have no `c_module()`, so do not include
this one; use `USER_C_MODULES`. On esp32 and rp2 point it at this checkout;
on a Make port point it at the directory that *contains* the checkout, which
builds every module in that directory.

MicroPython's
[manifest reference](https://docs.micropython.org/en/v1.29.0/reference/manifest.html)
explains `include()` and `c_module()`, and
[micropython-pydevices](https://github.com/PyDevices/micropython-pydevices)
keeps ready-made manifests and boards for several PyDevices modules at once.

## CircuitPython 11

Every module that is audiodsp's own builds into CircuitPython 11 as a user C
module from this repository's `micropython.mk`:

```sh
make BOARD=... USER_C_MODULES=/path/to/audiodsp
```

Run that in a CircuitPython port. Your sounds play through CircuitPython's
own `audiobusio`, `audioio` and `audiopwmio`; `audiocore`, `synthio`,
`audiomixer` and the effects modules stay CircuitPython's. Two modules differ
there: `audiopump` carries only `Ring` (see
[Play computed audio](../guides/play-computed-audio.md)), and `audiometer`
and `audiofeatures` aren't built, because their sources are a board's pump and
a USB sound card, and CircuitPython has neither.
