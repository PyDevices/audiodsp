# Contributing

Bugs, and things you need that don't work yet, go to
[issues](https://github.com/PyDevices/audiodsp/issues). Planned work is in
[ROADMAP.md](ROADMAP.md). A documentation correction, a focused CPython
fixture or a well-bounded module change is a good first pull request.

[How it works](docs/how-it-works/index.md) maps the repository: what each
directory holds, how one graph runs on three targets, and where the
boundaries are.

## Build from source

For CPython, build and install the wheel from a checkout:

```sh
python -m pip install build numpy
python -m build
python -m pip install dist/*.whl
```

For MicroPython and CircuitPython, build the way
[Install](docs/get-started/install.md) describes, with your checkout in place
of a clone. The `mp3` decoder that `audiomp3` wraps is fetched, not
vendored:

```sh
scripts/fetch_deps.sh
```

It puts the decoder under `.deps/` at the commit [DEPENDENCIES.lock](DEPENDENCIES.lock)
pins.

## Test

CI runs the CPython fixture tests and the parity gates whose goldens live in
this repository:

```sh
python -m unittest discover -s tests -p "test_*.py"
python tests/parity/verify_acceptance.py
python tests/parity/verify_effects.py
python tests/parity/verify_streaming.py
python tests/parity/verify_biquad.py
python tests/parity/verify_mixdown_knee.py
python -m flake8
```

Run the tests for the module you changed rather than the whole suite: the
full run is long. The deeper parity checks need built interpreters and the
pinned CircuitPython oracle beside this checkout, so CI doesn't run them and
you don't need them for a first contribution.
[The correctness standard](docs/how-it-works/correctness-standard.md) says
what each module is held to.

## Rules that keep the port honest

- **Don't modify the CircuitPython oracle.** The tag in `CIRCUITPYTHON_ORACLE`
  is what every parity golden is measured against. Edit CircuitPython's own
  sources for it and parity quietly means something else. Moving the pin is a
  resync with upstream, done deliberately: see
  [Keeping in step with upstream](docs/how-it-works/upstream-sync.md).
- **Extend CircuitPython, don't change it.** New modules live in `src/` and
  build into CircuitPython as a user C module through `circuitpython.mk`. They
  never shadow or rewrite one of CircuitPython's.
- **Don't re-capture a parity golden casually.** A fix that is right can still
  change one; say what independent evidence shows it is right.
- **Don't move the dependency pins** (`DEPENDENCIES.lock`, `CIRCUITPYTHON_ORACLE`)
  without saying why.
- **A fix for a bug that also exists upstream** goes in this repository's
  targets only, recorded in
  [upstream-diff](docs/how-it-works/upstream-diff.md); open an issue before
  adding a new deviation from CircuitPython. The fixes already sent upstream
  are in [upstream reports](docs/upstream-reports/README.md).

## Build and release wheels

`pydevices-audiodsp` is a native CPython distribution. `python -m build` makes
a local source archive and wheel.

A GitHub Release `vX.Y.Z`, matching the `VERSION` file (which is also where
`_audiodsp.__version__` comes from), starts the organization's reusable
publishing workflow. The pin is the `@publishing-vN` ref in
`.github/workflows/publish-release-packages.yml`, with
`build-kind: native-and-wasm`. It builds and validates exactly the number of
wheels `expected-wheel-count` names in that file. If the count changes, change
it in the same pull request.
[Wheels and platforms](docs/how-it-works/wheels-and-platforms.md) lists what
that is and what it leaves out.

Final releases then go to production PyPI too, after a maintainer approves the
`pypi` environment. That upload is the `pypi` job in this repository's own
`publish-release-packages.yml`, not in the reusable workflow, because PyPI's
Trusted Publishing only matches the workflow that runs the upload
([why](https://github.com/PyDevices/.github/blob/main/docs/publishing-automation.md#production-pypi-opt-in-protected-trusted-publishing)).

After TestPyPI publication, run `python scripts/test_testpypi_install.py` on
fresh Linux and Windows hosts. It takes an optional version and defaults to
the one in `VERSION`; it builds a bare environment and one with the `[render]`
extra.

## License

audiodsp is MIT licensed; see [LICENSE](LICENSE).

The attribution that goes with it lives in [NOTICE](NOTICE). Substantial
portions of this code are ported from CircuitPython under its own MIT
license. CircuitPython is a trademark of Adafruit Industries, and this
project is not affiliated with or endorsed by them. The vendored test corpus
under [`tests/vendor/`](tests/vendor/) carries its own licenses. The two are separate files on
purpose: GitHub and PyPI only detect the license when LICENSE holds the MIT
text and nothing else, so please don't fold NOTICE back into it. `NOTICE` is
in setuptools' default `license-files` glob, so it ships in the wheel and
sdist alongside `LICENSE`.

The Helix MP3 decoder that `audiomp3` wraps is RPSL/RCSL-licensed, not MIT.
[`scripts/fetch_deps.sh`](scripts/fetch_deps.sh) fetches it, carried unmodified per upstream's own
terms; it isn't part of this repository. See
[upstream-diff](docs/how-it-works/upstream-diff.md).

The Tremor Vorbis decoder that `audiovorbis` wraps is in this repository, in
[third_party/tremor](third_party/tremor), under Xiph.Org's BSD-3-Clause
license ([COPYING](third_party/tremor/COPYING)), which travels with the
wheel.
