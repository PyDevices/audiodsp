# Wheels and platforms

What a release builds, which platforms are proven by CI and which aren't,
and why. The steps to build and release are in
[CONTRIBUTING.md](../../CONTRIBUTING.md#build-and-release-wheels).

## What a release builds

A release builds and validates exactly **24** wheels
(`expected-wheel-count` in `.github/workflows/publish-release-packages.yml`
is the number that counts; if this page and that value disagree, the workflow
is right):

- CPython 3.11 to 3.15, plus the free-threaded 3.15t, on manylinux_2_28
  x86_64, Windows AMD64 and macosx_arm64: 18.
- CPython 3.13 to 3.14, Android API 21 arm64_v8a and x86_64: 4.
- CPython 3.13 to 3.14, Pyodide wasm32: 2.

## macOS: CI-proven

macOS is enabled at the CI-proven tier: ship everything for macOS that CI
can prove. `test-cpython.yml` builds, installs and runs the unit, contract
and parity suite and `tools/validate_api.py` on `macos-latest`, and its
`macos-wheels` job runs the same cibuildwheel a release uses, compiling the
native extension for CPython 3.11 to 3.15 and 3.15t and running an import
smoke test in each wheel, on every push. It publishes nothing. Hardware audio
output on a real Mac is community-verified: a field report is what would
promote that claim.

The wheel is **`macosx_arm64` only**. The runner is Apple Silicon, and an
x86_64 or universal2 wheel cross-compiled there couldn't run its test
command. An untested wheel is a claim, not a capability, so an Intel Mac
field report is what would add x86_64.

## Linux aarch64: CI-proven, not shipped

`[tool.cibuildwheel.linux]` uses `archs = ["auto64"]`, the runner's own
64-bit architecture, with `manylinux_2_28` images for both x86_64 and
aarch64. Naming `["x86_64", "aarch64"]` explicitly would make the x86_64
release runner try to build aarch64 under QEMU, which no workflow configures.
`auto64` resolves to x86_64 on an x86_64 runner and aarch64 on an ARM one, so
one config is correct everywhere.

`test-cpython.yml` proves aarch64 on native ARM runners (`ubuntu-24.04-arm`),
not under QEMU, because the bugs worth catching are the ones that differ by
architecture, and an emulator reproduces those only by accident:

- the full matrix (build, install, unit and contract tests, `validate_api`
  and the parity verifiers) runs on aarch64 for CPython 3.11 to 3.14;
- a `linux-arm-wheels` job runs the same cibuildwheel a release uses,
  compiling the extension and running each wheel's import smoke test.
  Nothing is published.

A release still ships no aarch64 wheel. The organization's desktop wheel
matrix has no ARM cell, so shipping one means adding an ARM runner to that
matrix, cutting a new publishing tag, and moving this repository's pin and
`expected-wheel-count` together.

## The same bytes within one architecture

Bit-identical audio is required **within one CPU architecture, not across
them**. No tolerance is introduced anywhere: the gate stays exact, and an
architecture that genuinely differs is held to its own recorded baseline,
accepted deliberately with its evidence written beside it. That is a stricter
regression detector than a threshold.

aarch64 is the example. At first, `verify_effects` differed on `multitap` and
`pitchshift`, the two effects that interpolate along a delay line: a
compiler's fused multiply-add changes the rounding, and AArch64 fuses by
baseline where x86-64 doesn't without `-mfma`. Forbidding contraction in
every [`src/shared/`](../../src/shared/) file that computes in float closed it (issue
[#79](https://github.com/PyDevices/audiodsp/issues/79)), so aarch64's own
baseline was removed from [`tests/parity/golden/effects_component.json`](../../tests/parity/golden/effects_component.json): it is
held to the oracle hash itself, the same one x86_64 is held to.

## What isn't built

- **musllinux** stays skipped: it doubles the Linux matrix for the Alpine
  family. aarch64 is built because its users can be named (every Pi and 64-bit
  SBC); Alpine's can't. It remains in `pyproject.toml`'s cibuildwheel `skip`
  list.
- **win32** (32-bit Windows): every Windows target here is 64-bit, and
  nothing in this repository has been built or tested 32-bit. Also in the
  `skip` list.
- **MicroPython `.mpy`**: the native modules are firmware. MicroPython and
  CircuitPython build them from `src/` as a user module, so there is nothing
  for mip to carry. The pure-Python tier publishes to mip as source, because
  compiling it to `.mpy` would pin it to one bytecode version per MicroPython
  release.
