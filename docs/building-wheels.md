# Building and publishing wheels

`pydevices-audioif` is a native CPython distribution. Build a local source
archive and wheel with:

```sh
python -m build
```

A GitHub Release `vX.Y.Z` — matching the `VERSION` file, which is also where
`_audioif.__version__` comes from — invokes the organization `publishing-v8`
workflow (the pin is the `@publishing-v8` ref in
`.github/workflows/publish-release-packages.yml`) with
`build-kind: native-and-wasm`. It builds and validates exactly **21** wheels:

- CPython 3.10–3.14, manylinux_2_28 x86_64, Windows AMD64, and
  macosx_arm64 — 15.
- CPython 3.13–3.14, Android API 21 arm64_v8a/x86_64 and Pyodide wasm32 — 6.

(This paragraph said `publishing-v6` and 16 wheels until 2026-09-02, having
gone stale when macOS landed. The count that matters is
`expected-wheel-count` in `publish-release-packages.yml`; if this prose and
that value ever disagree again, **the workflow is right**.)

## macOS: enabled, CI-proven

macOS is enabled at the org's **CI-proven** tier ("PyDevices ships
everything for macOS that CI can prove" — see the organization
`platform-support-tiers.md`): the `test-cpython.yml` workflow builds,
installs, and runs the full unit/contract/parity suite and
`tools/validate_api.py` on `macos-latest`, and its `macos-wheels` job
runs the same cibuildwheel the release path uses — compiling the native
extension for CPython 3.10–3.14 and running the import smoke test in
each wheel — on every push, publishing nothing. Hardware audio output
on a real Mac remains **community-verified**: no Mac is on our bench,
and a field report is what promotes that claim.

**Arch choice: `macosx_arm64` only.** The runner is Apple Silicon; an
x86_64 (or universal2) wheel cross-compiled there could not run its
test-command, and an untested wheel is a claim, not a capability. An
Intel-Mac field report is what would add x86_64.

**The release-time gap is closed** (corrected 2026-09-02; this section
described it as open long after it shut). `macos-latest` joined the org
desktop matrix in **publishing-v7**, this repository is pinned at
**`@publishing-v8`**, and `expected-wheel-count` is already **21** — the
five `macosx_arm64` wheels ship. Hardware audio output on a real Mac is
still community-verified; that part has not changed.

## Linux aarch64: enabled, CI-proven; not yet shipped

**Decided 2026-09-02 (Brad): add Linux ARM.** aarch64 is the Raspberry Pi
and every 64-bit SBC — real users on a tier the organization's
`platform-support-tiers.md` already claims desktop-Linux coverage for,
while shipping no ARM wheel. That gap between claim and artifact is the
cause; "it would be nice" is not.

`[tool.cibuildwheel.linux]` uses **`archs = ["auto64"]`** — the runner's
own 64-bit architecture — with `manylinux_2_28` images for both x86_64 and
aarch64. Naming `["x86_64", "aarch64"]` explicitly would have been a
release-breaking mistake: the x86_64 release runner would then try to build
aarch64 under QEMU, which no workflow configures. `auto64` resolves to
x86_64 on an x86_64 runner and aarch64 on an ARM one, so one config is
correct everywhere and an ARM release cell needs no config change at all.

`test-cpython.yml` proves it two ways on **native ARM runners**
(`ubuntu-24.04-arm`) rather than under QEMU:

- the full matrix — build, install, unit/contract tests, `validate_api`,
  and all four parity verifiers — runs on aarch64 for CPython 3.10–3.14;
- a `linux-arm-wheels` job runs the same cibuildwheel the release path
  uses, compiling the extension and executing each wheel's import smoke
  test. Nothing is published.

**Native, not emulated, deliberately.** Under QEMU each wheel's
test-command runs on a faithful *simulation* of the target. The bugs
worth catching here are the ones that differ by architecture, which is
exactly the class an emulator reproduces only by accident. An untested
wheel is a claim; a wheel tested on emulated hardware is a weaker claim
than it looks.

**What the ARM lane found on its first run, and what closed it.** aarch64
reproduces audioif's effects output byte for byte, since 2026-09-17. It did
not at first, and the gap is the part worth keeping. Three of the four
parity gates (`verify_acceptance`, `verify_streaming`, `verify_biquad`)
were identical from the start; `verify_effects` was not — **6 of 744
numeric fields differed, largest absolute deviation 1 in every case**,
confined to `multitap` and `pitchshift`, the two effects doing delay-line
interpolation. That is where a compiler's fused multiply-add changes the
rounding, and AArch64 fuses by baseline where x86-64 does not without
`-mfma`. So the six blocks were accepted as an aarch64 baseline of its own,
with the evidence written beside it.

[#79](https://github.com/PyDevices/audioif/issues/79) forbade contraction
in every `src/shared/` file that computes in float, and the six blocks
closed. Eight `ubuntu-24.04-arm` cells — Python 3.11 through 3.14 in two CI
runs — now hash identically to the x86_64 reference, **0 of 882 numeric
fields differing**, checksums included rather than sums alone. aarch64's
baseline is therefore *removed* from
`tests/parity/golden/effects_component.json` rather than updated: it is
held to the oracle hash itself now, the same one x86_64 is held to, and
`cpython_stdout_sha256_reproduced_by` records that the agreement was
measured so a future mismatch reads as a regression.

The rule this settled (Brad, 2026-09-02) stands whatever any one
architecture does: **bit-identical audio is required within one CPU
architecture, not across them.** No tolerance was introduced anywhere. The
gate stays exact everywhere, and an architecture that genuinely differs is
held to its own recorded baseline, accepted deliberately with its evidence
written beside it. That is both a faithful reading of the rule and a
stricter regression detector than a threshold.

**Release-time gap, precisely — this one IS open.** The organization
desktop matrix (`reusable-build-native-and-wasm-wheels.yml`, at
`publishing-v8`) runs `ubuntu-latest`, `windows-latest` and
`macos-latest`. There is no ARM cell, so **a release still ships no
aarch64 wheel** and `expected-wheel-count` stays 21. Closing it is three
coupled steps, all outside this repository:

1. add an ARM runner to that matrix (the macOS comment there describes
   the same shape: repos that skip the arch build a no-op cell);
2. cut the new `publishing-vN` tag;
3. move this repository's pin and raise `expected-wheel-count` 21 → 26
   in the same change.

Steps 2 and 3 are release-machinery version decisions and belong to Brad.
The work here means that when they happen, the ARM lane is a matrix line
rather than a discovery.

## The remaining exclusions

Each with its cause rather than by silence:

- **musllinux** — **decided 2026-09-02 (Brad): stay skipped.** It doubles
  the Linux matrix for the Alpine family, and the test that admitted
  aarch64 is what excludes it: we can name aarch64's users (every Pi and
  SBC, on a tier we already claim) and cannot name musllinux's. Revisit
  if an actual Alpine user asks. Remains in `pyproject.toml`'s
  cibuildwheel `skip` list.
- **win32** (32-bit Windows) — every Windows target here is 64-bit: the
  wheel matrix builds AMD64 only and the validation hosts are 64-bit;
  nothing in this repository has ever been built or tested 32-bit. Also in
  the cibuildwheel `skip` list.
- **Linux ARM** — no longer an exclusion; see the aarch64 section above.
  The old note here said ARM would need QEMU on x86_64 runners. GitHub now
  offers native `ubuntu-24.04-arm` runners to public repositories, so the
  premise that made it expensive is gone.
- **MicroPython `.mpy`** — the native modules are firmware: MicroPython and
  CircuitPython build them from `src/` as a usermod, so there is nothing
  for MIP to carry (recorded in `publish-release-packages.yml`). The
  pure-Python tier publishes to MIP as source; compiling it to `.mpy`
  would pin it to one bytecode version per MicroPython release, and no
  decision to take that on has been made.

After TestPyPI publication, run
`python scripts/test_testpypi_install.py` on fresh Linux and Windows hosts.
