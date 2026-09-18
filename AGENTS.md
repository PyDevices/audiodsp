# AGENTS.md — audioif

CircuitPython's audio system (`audiocore`, `synthio`, `audiomixer`, effects,
`audiomp3`), ported to MicroPython as `USER_C_MODULES` usermods. Import
names match CircuitPython exactly (`import audiocore`, `import synthio`, …)
for source compatibility; only this repo's own name differs.

## Layout

- Root: `micropython.mk` (unix/windows, Make-based ports), `micropython.cmake`
  (esp32/rp2, CMake-based ports) — build glue for `USER_C_MODULES` discovery
- `src/` — one directory per module (`audiocore/`, `synthio/`, `audiomixer/`,
  `audiospeed/`, `audiofreeverb/`, `audiofilters/`, `audiodelays/`,
  `audiomp3/`, `audiodynamics/`, `audioroute/`, `audiomath/`, `audioecho/`,
  `audioconvolve/`, `audiobiquad/`, `audioshaper/`, `audioladder/`,
  `audioverb/`, `audiomodal/`), plus `src/cp_compat/`
  (CircuitPython-only core primitives ported as standalone compat shims, each
  individually verified against mainline MicroPython before use — not assumed
  missing) and `src/shared/` (runtime-neutral DSP the MicroPython usermod and
  the CPython extension both compile)
- `src/cpython/` — the whole CPython target: `_audioif.c`, the extension built
  in place, and the thirteen modules that wrap it (`audiocore.py`,
  `synthio.py`,
  …). They install as top-level modules, so it is `import audiocore` no matter
  which of the three runtimes is underneath. Nothing puts this directory on
  `sys.path`: audioif is a dependency, imported from wherever it is installed.
- `lib/` — the pure-Python tier: `lib/audiorender/` (whole-composition
  offline rendering — numpy, desktop-only, never frozen) and
  `lib/audioif_util/` (`float32`, the round trip that makes a setting derived
  in Python the same number on a board as on a desktop — see
  [docs/correctness-standard.md](docs/correctness-standard.md)). Both ship
  inside the `pydevices-audioif` wheel. The instrument and effect libraries that
  used to sit beside it — `audioinstruments` (53 `synthio` instruments) and
  `audioeffects` (46 effect classes, racks included) — live in
  [audiocomponents](https://github.com/PyDevices/audiocomponents) now, as
  their own distributions depending on `pydevices-audioif`; nothing in this
  repository builds, tests, publishes or freezes them.
- `apply_cp_patches.sh` + `src/circuitpython_spike/` — add `audiodynamics`,
  `audioroute`, `audiomath`, `audioecho`, `audioshaper`, `audioladder`,
  `audioconvolve`, `audiobiquad`, `audioverb` and `audiomodal` to a
  CircuitPython tree. None of the ten is a CircuitPython port: the first
  two come from micropython-vst3's `vstaudio` engine and the last eight are
  audioif's own, so CircuitPython gains them here rather than the other
  way round.
- `docs/porting-plan.md` — the full phased porting history, architecture,
  and target layout
- `docs/upstream-diff.md` — every deliberate deviation from upstream
  CircuitPython, with rationale (verbatim-kept quirks vs. genuine port bugs
  found and fixed)
- `tests/parity/` — oracle-diff scripts, run unchanged against both this
  port and `bin/circuitpython`
- `tests/vendor/` — vendored third-party test fixtures (e.g. `synthtools`)

## Sibling dependencies (cloned, not vendored)

- `ulab` (numpy-alike) — pinned to the exact revision CircuitPython itself
  vendors
- `cmods/mp3` (upstream `adafruit/Adafruit_MP3`, the Helix MP3 decoder core
  `audiomp3` wraps) — RPSL 1.0/RCSL 1.0 licensed, *not* MIT; carried
  unmodified per upstream's own terms, same as CircuitPython itself. Kept
  as a separate sibling clone rather than folded into this (MIT) repo's own
  tree.

Both are expected as siblings in the parent workspace (`cmods/` in
`pydevices`), same pattern as `pygraphics`/`displayif`.

## Testing

- Oracle diffing is the core discipline: every parity script under
  `tests/parity/` runs unchanged against this port and `bin/circuitpython`,
  rendering PCM and diffing byte-for-byte (or documenting the exact,
  bounded exception in `docs/upstream-diff.md`).
- The nine modules that are **ours** have no oracle, by decision
  ([docs/correctness-standard.md](docs/correctness-standard.md), 2026-09-09).
  Upstream CircuitPython has no counterpart to them, so they are held to two
  things: every target renders them identically, and their numeric traits hold.
  `.venv/bin/python tests/parity/verify_dsp.py --micropython ../cmods/micropython/ports/unix/build-standard/micropython`
  is the first. There is **no stored digest** — the gate is the comparison, and
  it **refuses a run with fewer than two interpreters** rather than passing one
  that cannot fail. Add `--circuitpython ../cmods/bin/circuitpython-oracle-10.3.0`
  for the three-way. The arithmetic is all in `src/shared/`, so two
  interpreters disagreeing is never a difference of intent: it is a width, an
  undefined shift, a compiler's choice or an architecture.
- The traits are the other half, one file per module —
  `tests/test_cpython_<module>.py`, each opening with its trait table and the
  bar for every row. Agreement cannot see a change that moves all three targets
  together; the traits can, because they are measured against arithmetic rather
  than against our own last answer. `audiobiquad`'s probe also prints two
  invariants as integers beside its PCM (the block at which a tail reaches
  exact zero, and the null depth at each feedback value), because a comparison
  over PCM alone would not say whether either still held.
- **The vstaudio oracle is retired.** `audiodynamics` and `audioroute` came
  from micropython-vst3's engine and used to be held to `vstaudio_dsp.c`
  compiled unmodified. That file was deleted from micropython-vst3 in `6ea60d3`
  and the plug-in links audioif now: the relationship reversed, so the engine is
  a consumer of this package rather than a grader of it. The build script, the
  usermod and `golden/dsp_nodes.json` are gone.
- The instruments parity gate — `run_instruments_parity.py`, its two probes,
  `instrument_sequences.py` and the `instruments_*.json` digests — lives in
  [audiocomponents](https://github.com/PyDevices/audiocomponents) now, under
  its `tests/parity/`, beside the packages it renders. Its `REBUILT` rule (a
  name there records a sound changed **on purpose**, and adding one is
  Brad's call, never an agent's) is documented in that repository's
  AGENTS.md. It is not run from here.
- What covers the mpvst cutover is `python3 tests/parity/capture_render_reference.py
  --verify`: it renders every piece of [mpvst](https://github.com/PyDevices/mpvst)'s
  soundtrack and compares each with what it sounded like beforehand. It drives
  `examples/soundtrack/composer/preview.py` in an mpvst checkout — `--mpvst`
  points at one, default `../mpvst`. The interpreter it renders with needs
  `audioinstruments` and `audioeffects` installed from audiocomponents (or
  `--components-lib <checkout>/lib`); they are no longer in this tree. Slow
  (~30 min) and not part of the default gate, but **re-capture it after any
  DSP change here** or it stops meaning anything. The stored golden holds
  seven pieces against the state of the workspace on 2026-09-03; the
  soundtrack has grown since, and a piece the golden has never seen is
  reported as new rather than compared. A piece that cannot be rendered at all
  is reported and the run carries on — as of 2026-09-18 two of them cannot,
  because their effect racks predate audiocomponents' effects rebuild
  ([mpvst#8](https://github.com/PyDevices/mpvst/issues/8)). Repointed in
  audioif#88 — the renderer it used to drive,
  `micropython-vst3/tools/render_preview.py`, was deleted when both composers
  moved beside their songs.
- `python3 -m flake8` is the lint gate (`.flake8`, defect checks only —
  layout is deliberately not gated). It runs in CI on every push.
- Full regression after any change: rebuild interpreters
  (`build_interpreters.sh` in the parent workspace), run the tier 0-5
  parity suite plus `tests/parity/synthtools_acceptance.py`, and the LVGL
  smoke test.
- See `docs/porting-plan.md`'s "Testing strategy" section for the complete
  methodology.
- **The parity/oracle gates are workspace-local by design.** They need
  built interpreters (`bin/circuitpython`, the workspace MicroPython) and
  golden captures that live outside this repository, so external
  contributors cannot run them and CI does not try. **Since 2026-09-03 the
  scripts' zero-argument defaults no longer point into `cmods/`** — a
  standalone user should not be steered into a directory only this
  workspace has — so in *this* workspace every parity command needs the
  explicit `--micropython`/`--circuitpython` (or `CP_DIR`, `MP_UNIX`,
  `ULAB_DIR`) override shown above. `verify_dsp.py` used to print `skipping
  micropython (not built at ...)` and carry on with whatever was left, which
  was not a pass; it now **refuses** a run with fewer than two interpreters,
  because one interpreter cannot disagree with itself and a run that cannot
  fail is worse than no run. What CI
  covers instead is what needs only the wheel: the CPython fixture tests in
  `tests/test_cpython_*.py` and the five in-repo parity gates
  (`verify_acceptance`, `verify_effects`, `verify_streaming`,
  `verify_biquad`, `verify_mixdown_knee`), whose goldens are committed
  here. The fifth (2026-09-06, audioif#27) is the only one whose material
  crosses the mix-down limiter's +/-28000 knee, so it is the only one that
  can see a synthio voice-ceiling change at all — the other four are
  byte-identical at every ceiling value. It sees exactly one of the
  ceiling's five sites, `src/cpython/synthio.py`'s `max_polyphony`, because
  that is the only one the CPython target reads; `tests/
  test_voice_ceiling_consistency.py` is still the guard for a half-applied
  change. Its above-knee half is enforced against **this port**, not
  against the oracle: the port is built at 64 and the pinned oracle at 14,
  and no above-knee material can agree across those two. The component
  contract tests (`test_audio_component_api`, `test_metadata_contract`,
  `tools/validate_api.py`) went to audiocomponents with the packages they
  check.
- **Two kinds of golden, two rules (Brad, 2026-09-03).** The instruments
  digests — `instruments_*.json`, now under audiocomponents'
  `tests/parity/golden/` — record that *the port matches the pre-rewrite
  original script, within one interpreter*: `run_instruments_parity.py`
  there renders the originals from micropython-vst3 at `DEFAULT_OLD_REV` and
  never consults the CircuitPython oracle. So a change to `src/cpython/`
  *here* that is *right* still stales those cpython digests (audioif#25:
  `b420dac` did exactly this). Rule: a CPython-target fix may re-capture the
  affected cpython digests **only if it carries independent evidence against
  the built oracle** — a test in `tests/test_cpython_*.py` run against
  `bin/circuitpython` and cited in the message. The digests living in the
  other repository changes only the mechanics: the audiocomponents
  re-capture names the audioif commit that carries that evidence. A fix that
  merely *asserts* oracle intent does not qualify; that would let it rewrite
  its own reference. The accuracy program's *listening* goldens
  (audiocomponents) are a different authority — Brad's ear — and move only
  at his phrase. The stored digest is also the only thing in that gate that
  notices the engine moving under both original and port — which is why it
  stays: a live original-vs-port comparison was measured, approved and then
  reversed the same day (audioif#26), because it would have stayed green
  through `b420dac`. Each alarm costs one adjudication; that is the price of
  the signal.

## The CircuitPython oracle — extend, never modify

`cmods/circuitpython` (sibling checkout, detached at tag `10.3.0`) is the
**oracle** every parity golden is measured against. The rule, for any agent
working here:

- **The oracle binary is `cmods/bin/circuitpython-oracle-<version>`**, built
  only by `cmods/build_interpreters.sh --only cp-oracle` (CircuitPython's unix
  coverage variant at `CIRCUITPY_SYNTHIO_MAX_CHANNELS=64`) and re-pinned in
  `tests/test_voice_ceiling_consistency.py` in the same change that builds it.
  **`cmods/bin/circuitpython` is not the oracle** — it is what that script's
  `cp-unix` target installs, at the coverage variant's own 14-voice ceiling,
  and it changes under you whenever anyone refreshes the interpreters
  (audioif#89, twice in eight days).
- **Never edit files in `cmods/circuitpython` directly.** A modified oracle
  silently redefines what "parity" means and invalidates every golden without
  failing anything. Its *pin* is a different matter: it moves when this port
  moves to a new CircuitPython release, deliberately, in a change that re-reads
  every CP-shared gate and re-pins the binary's hash with the reason written
  down. The oracle is built at the same version and the same ceiling this port
  ships — an oracle at a different configuration cannot answer the only question
  worth asking. See `docs/correctness-standard.md` and
  `tests/test_voice_ceiling_consistency.py`.
- **Extending CircuitPython is fine and is the designed path**: new modules
  live in this repo under `src/circuitpython_spike/`, and
  `apply_cp_patches.sh` copies them (plus `src/shared/` DSP) into the CP
  tree. The script is **additive-only by design** — it adds files and
  registers them in build glue; the sole stock-file rewrite it performs is
  the fenced audiocore `'B'`-memoryview patch. Do not add non-additive
  rewrites to it: `cmods/build_cp.sh` auto-applies the script before
  building `bin/circuitpython`, so a behavioral rewrite would leak *into*
  the oracle.
- Fixes to bugs that also exist upstream go in **this repo's targets only**
  (MicroPython/CPython/`src/shared/`), recorded in `docs/upstream-diff.md`
  — never into the CP tree. Approved deviations from the oracle are
  enumerated there; **ask before adding one**.
- Quick self-check after any CP-adjacent work:
  `CP_DIR=../cmods/circuitpython ./apply_cp_patches.sh --status` must account
  for every difference (bare, the script no longer finds the tree here and
  says `CircuitPython tree not found (set CP_DIR)`), and
  `git -C ../cmods/circuitpython status` must show only the known additive
  set (new module dirs, build glue, the fenced audiocore rewrite) — no
  changes under `shared-module/`/`shared-bindings/` for `synthio`,
  `audiofilters`, `audiocore` (beyond the fence), `audiomixer`, or
  `audiodelays`.
