# What audiodsp is held to

Decided with Brad on 2026-09-09, replacing every earlier arrangement. One page,
because the previous answer was spread across a golden file, an oracle binary,
a build script, three deviation sections and a rule about a binary nobody was
allowed to rebuild — and nobody could state it in a sentence.

## The rule

**A node that CircuitPython also has must render the same bytes CircuitPython
renders, under the same conditions.** Same sample rate, same channel count,
same options, and the same compile-time configuration — this port ships
`CIRCUITPY_SYNTHIO_MAX_CHANNELS=64`, so the CircuitPython build we compare
against is built at 64 as well, and the comparison is only meaningful because it
is. Differences in configuration are not findings; they are setup errors.

**A node that is ours alone has no such reference, so it is held to two things
instead:** that all three of our targets render it identically, and that its
behaviour meets stated numeric traits. Neither is a stored byte.

Nothing here is compared against a previous version of our own output.

## Why not our own past

Because it is not a standard, it is a memory of a decision. Three ways it fails,
all of them observed in this repository rather than imagined:

* **A stored digest can be re-blessed.** `--capture` moves both sides of the
  comparison at once, the gate goes green, and nothing was checked. That is
  audiodsp#27, and `verify_mixdown_knee.py`'s anti-launder tripwire exists
  because of it.
* **A stored digest can be green while the code is wrong.** On 2026-09-09 the
  MicroPython flanger overflowed `int32_t` in its wet interpolation on
  full-scale material and disagreed with the CPython twin on every setting
  tried. Every stored fixture in `flanger_probe.py` matched to the byte
  throughout. Only comparing the two targets against each other found it.
* **A reference can outlive the thing it referenced.** The vstaudio oracle
  pinned `audiodynamics` and `audioroute` to micropython-vst3's
  `vstaudio_dsp.c` at revision `ac87f13`. That file was deleted from
  micropython-vst3 in `6ea60d3` — "Drop the effects library and the DSP that
  moved to audiodsp" — and the plug-in links audiodsp now. The relationship
  reversed: vstaudio is a consumer of this package, not a grader of it.

## Which nodes are which

The dividing line is not "did we write the C". It is whether **upstream
CircuitPython** ships the module. Both kinds are present in
`cmods/circuitpython`, and `git ls-files` tells them apart: ours are patched in
by `apply_cp_patches.sh` and are untracked there.

| CircuitPython's own — held to CP's bytes | Ours alone — no external reference |
|---|---|
| `audiocore` | `audiobiquad` |
| `audiodelays` | `audioconvolve` |
| `audiofilters` | `audiodynamics` |
| `audiofreeverb` | `audioecho` |
| `audiomixer` | `audioladder` |
| `audiomp3` | `audiomath` |
| `audiospeed` | `audioroute` |
| `synthio` | `audioshaper` |
| | `audioverb` |

Nine of the seventeen are ours, and they are most of the effects palette — which
is why "match CircuitPython" cannot be the only rule, and why the second half of
this page is not an afterthought.

Two of ours came from micropython-vst3's engine (`audiodynamics`, `audioroute`)
and the rest have no ancestor anywhere. Neither fact changes anything: what
matters is that no independent implementation exists to compare against today.

## What holds our own nodes up

**1. Three-target agreement, with nothing stored.** The CPython extension,
desktop MicroPython, and the patched CircuitPython build must render a probe
identically. All three compile the same kernel under `src/shared/`, so this is
a real check on the three compilations and on anything that is
architecture-dependent, undefined, or width-sensitive — the class of defect that
found the flanger overflow. It cannot be laundered, because there is no file to
re-capture.

**Its honest limit, stated so nobody relies on it for more than it gives:** a
change to the shared kernel moves all three together and stays green. Three-way
agreement sees divergence between targets. It cannot see drift over time. That
is what the next item is for.

**2. Numeric traits with planted faults.** A stated property with a bar —
"a 100 Hz low-pass attenuates 1 kHz by at least N dB", "a reset node renders
exact zero on silence from its first block", "the settled reduction at ratio R
is 20·(1−1/R) dB ± 0.01" — and beside it a deliberately broken build that must
fail the check. This is measuring against physics and arithmetic, which survive a
refactor and do not survive a regression. It is the effects program's own method
and it is the right one here.

A trait without a planted fault is not a check; it is a hope. See
`docs/program-pattern.md` §4 in the anchor, and `tests/parity/deinit_surface_probe.py`
for the shape — its `--fault` mode is run by CI first, and CI fails if the fault
*passes*.

## What this retires

Agreed 2026-09-09; the removals follow this page rather than precede it, so
until each line below is struck the old machinery is still present. This list
is the checklist, not a report.


* ~~**The vstaudio oracle**~~ — **done.** `tests/parity/vstaudio_oracle/`,
  `tests/parity/build_vstaudio_oracle.sh` and the pinned `VSTAUDIO_REV` are
  deleted, along with `golden/dsp_nodes.json`. `verify_dsp.py` is now the
  comparison itself: it runs every probe on every interpreter given and requires
  the outputs to be byte-identical, with **no stored digest**, and it **refuses
  a run with fewer than two interpreters** rather than passing one that cannot
  fail. A probe that cannot be covered yet is `PENDING` against a named issue -
  visible and counted, neither silently skipped nor standing red. Proved able to
  fail: reverting the flanger's 64-bit widening and rebuilding gives
  `FAIL flanger_probe.py cpython and micropython differ at output byte 1160`.

  The gate moved workflow with its meaning. It needs two interpreters, so it
  runs in `clean-build.yml`, which builds one. `test-cpython.yml` and the
  release job keep what a single interpreter *can* say - that every probe runs -
  across four operating systems and five Pythons, which catches an import
  error or an arithmetic assumption that only holds on x86_64.
* ~~**`cmods/bin/circuitpython` as an untouchable artefact**~~ — **done**
  2026-09-09. The oracle is now built at the version and the ceiling this port
  ships (CircuitPython 10.3.0, `CIRCUITPY_SYNTHIO_MAX_CHANNELS=64`) and is
  rebuilt deliberately whenever either moves, re-pinned in the same change with
  the reason written down. `cmods/build_cp.sh` passes `CP_CFLAGS_EXTRA` through
  for the ceiling override, which must be `-U` then `-D` because `-Werror` makes
  a conflicting redefinition an error. `tests/test_voice_ceiling_consistency.py`
  still compares the binary's bytes against a pin — that check is about noticing
  an *undeclared* rebuild and is not retired. The oracle moved to
  `cmods/bin/circuitpython-oracle-10.3.0` on 2026-09-17 (audiodsp#89): the old
  path is what `build_interpreters.sh`'s `cp-unix` target installs, so a
  routine interpreter refresh replaced the oracle with a 14-voice build twice.
  The opt-in `cp-oracle` target builds and installs the oracle now, and
  nothing else writes that path.

  The rebuild earned itself immediately: it found two behaviour changes
  CircuitPython 10.3.0 made to `synthio` and `audiomixer` that this port had not
  taken — the panning polarity flip and the zero-crossing loudness gate. Both
  were quiet arithmetic edits rather than new nodes, so the port already carried
  10.3.0's new *files* and looked current. Nothing that grades us against our own
  past could have seen them.
* ~~**The 14 → 64 voice-ceiling deviation**~~ — **done** 2026-09-09, and gone
  rather than documented. Both sides are built at 64,
  `SYNTHIO_MIX_DOWN_SCALE` is 129 on both, and `verify_mixdown_knee` is
  byte-identical to CircuitPython above the knee as well as below.
* ~~**Stored digests as a reference of record**~~ — **done** for the DSP nodes:
  `golden/dsp_nodes.json` is deleted and `verify_dsp` compares interpreters
  against each other. The CP-shared gates (`verify_acceptance`,
  `verify_effects`, `verify_streaming`, `verify_biquad`, `verify_mixdown_knee`)
  still carry stored files, and those are a different case: they hold this port
  to *CircuitPython's* answer, which is the rule, not to our own past. Probes
  stay — they are how a
  render is made reproducible and comparable. What goes is treating the stored
  value as the thing that must be matched, rather than as a convenience for
  running the comparison.

`docs/upstream-diff.md` does **not** retire. Deliberate departures from
CircuitPython in nodes CircuitPython has are exactly what it is for, and under
this rule each one is a decision that must be written there or it is a bug.

## What a divergence means now

* **CP-shared node, we differ from CircuitPython.** Either a bug of ours, or a
  bug of theirs worth an upstream report (`docs/upstream-reports/`), or a
  deliberate departure that goes in `docs/upstream-diff.md`. Three outcomes, and
  choosing between them is the work. What it is never is "re-record ours".
* **Our own node, the three targets disagree.** Ours, always. One of the three
  compilations is wrong, or the kernel relies on something undefined.
* **Our own node, a trait fails.** Ours, and the trait is the specification —
  unless the trait was wrong, in which case say so, change it, and say why.

## The one way the same C fails this on its own

Three targets compiling one file is the whole mechanism, so anything that lets a
compiler choose its own arithmetic breaks the gate without anybody writing a
bug. There is one such thing and it is dealt with: **fused multiply-add**.
`a * b + c` may round twice or once, both are legal, and which one you get
depends on the target -- the two ESP toolchains do not even fuse at the same
number of sites in the same file. That was the whole of the P4-vs-S3 split on
`audiodynamics` (audiodsp#66), and since audiodsp#79 every `src/shared/` file that
computes in float includes `shared/audiodsp_fp_contract.h` and forbids it.

The header is where the reasoning lives: the two spellings that silently do
nothing, why it is not a build flag, and what it costs. **Add the include to any
new shared file that does float arithmetic**, or that file alone keeps the
behaviour the rest of the kernel has given up.

This gate cannot see contraction *on an x86-64 host*, and should not be
trusted to. x86-64 does not fuse without `-mfma`, so all three desktop
interpreters there agree whether or not the pragma is present, and that is the
host almost every run happens on. aarch64 is not blind to it: AArch64 fuses by
baseline, and CI's ARM lane had carried an accepted baseline of its own since
2026-09-02 for six `multitap` and `pitchshift` blocks one byte from x86_64.
Adding the pragma closed exactly those six, which is where the mechanism stops
being inferred and starts being measured
([docs/building-wheels.md](building-wheels.md)). Proving the board half still
needs board digests. See audiodsp#79 and audiodsp#55.

## The other way: a number derived in Python

Contraction is the compiler choosing. This one is the *interpreter* choosing,
and it happens before the kernel is reached at all.

**A setting derived in Python passes through `audiodsp_util.float32` before it
reaches a node.** That is the rule; the rest of this section is why.

Python's float is the interpreter's `mp_float_t`. On CPython and on the desktop
MicroPython that is a double; on an ESP32-P4, an ESP32-S3, an RP2040 and any
build carrying `-DMICROPY_FLOAT_IMPL=MICROPY_FLOAT_IMPL_FLOAT` it is a single.
So `node.mix = 0.35` is not one setting -- it is two numbers a ULP apart -- and
a node whose blend runs from it renders different bytes on a board than on a
desktop without anything in the kernel being wrong. `audiodsp_util.float32(x)` is
a pure-Python round trip through `struct`: the identity on a single-precision
target, a rounding on a double one, and the same number afterwards on both.

`audiodsp_util.float32_bits(x)` is the same rule for output. `"%.6f" % value` is
seven significant digits, and MicroPython's single-precision formatter is not
correctly rounded that far, so a probe printing a float at that width compares
formatters rather than DSP. The bit pattern is exact everywhere.

This is where audiodsp#80 landed: six probes disagreed with CPython on a
single-precision build, and four of the six were the probes' own arithmetic --
three printing a gain reduction whose *bits were identical on all three
targets*, one passing `mix=0.35`, and one deriving its square-wave material from
a Python float phase, so the two builds were being compared on two different
input signals. The two that remain are below the probes and each has its own
issue: audiodsp#101 (`synthio.Biquad` derives W0 at `mp_float_t` width where the
CPython extension calls the shared `double` helper) and audiodsp#102 (a filtered
`Echo` does its per-sample arithmetic at `mp_float_t` width in C and in double
in the twin).

**The same rule belongs to the classes.** audiocomponents#75 is this shape one
tier up -- a class computing `360 * 4 ** (macro / 127)` for a filter frequency
lands its board and its desktop a ULP apart, and the effects programme's board
proofs of 2026-09-17 name it as one of the three causes of a drive class's
digests differing. The derivation ends in `float32` there too, or the board and
the desktop are not running the same filter.

## The third way: the twin keeping what the native borrows

Contraction is the compiler choosing, a derived setting is the interpreter
choosing. This one is neither: it is the CPython twin owning memory the native
builds only point at.

`audiosample_get_buffer` hands its caller a **pointer into the producer's own
buffer** — `int16_t buffer[AUDIODSP_..._FRAMES * 2]` on the node, one buffer, not
a queue. A consumer that holds that pointer across calls therefore reads what
the producer rendered *last*, not what it had rendered when the pointer was
taken. `audiomixer.MixerVoice` is exactly such a consumer: `play()` fetches one
block and the mix-down reads it later.

The twin has no pointers, so it answered each pull with a fresh `bytes` and a
borrowed block could not be overtaken. Any class that pulls a node one of its
own mixer voices is already holding then rendered one block differently on
CPython than on every native build — which is what
[`audioeffects.rebuilt.Saturation`](https://github.com/PyDevices/audiocomponents)
does to settle its coupling pole before its first block, and why its two
`Bias`-off-centre patches split CPython from desktop MicroPython, desktop
CircuitPython and both boards (audiodsp#89).

`audiocore._AudioSample._publish` is the rule now: a node hands back its own
buffer, refilled — one slot for the nodes audiodsp wrote, two for the ported
CircuitPython effects, for `Mixer` and for the synthesizer, because those
alternate. `audiocore._borrow` is the in-graph pull that does not copy;
`audiocore.get_buffer` still copies, on both targets, because it is the
script-facing one. `tests/parity/mixer_borrowed_block_probe.py` is the gate.

**What this does not yet cover:** a node's *leftover* source block — `pending`
in `src/audiobiquad/Biquad.c` and its siblings, kept when one source buffer
outlasts one render — is a borrowed pointer in the C and still a copy in the
twin. Nothing has been measured to depend on it; it is named here so the next
divergence of this shape is recognised rather than re-derived.

## The fourth way: `mp_float_t` inside the kernel, which no rule can round away

The two ways above have fixes. A compiler choosing contraction is forbidden by
a pragma; a number derived in Python is put through `audiodsp_util.float32`
before it reaches a node. This one has neither, and **that is a decision rather
than an omission** (Brad, 2026-09-22, decision 4 of the housekeeping sweep:
accept and document, do not fix).

The shape is always the same. A binding computes something in `mp_float_t` and
hands it to a kernel declared in `double`. `mp_float_t` is a double on a
desktop and a **single** on every board we ship, so the number that arrives has
already been rounded differently before any kernel runs, and nothing downstream
can recover it. Six issues, one mechanism:

| issue | where | the value |
|---|---|---|
| [#101](https://github.com/PyDevices/audiodsp/issues/101) | `synthio/Biquad.c` derives `W0 = frequency * synthio_global_W_scale` | the biquad's centre frequency |
| [#102](https://github.com/PyDevices/audiodsp/issues/102) | `audiodelays/Echo.c` computes `echo * decay + sample` | a filtered echo's feedback sum |
| [#103](https://github.com/PyDevices/audiodsp/issues/103) | `audiofilters/Distortion.c`'s `db_to_linear` | a dB setting's linear gain, and on a board a different libm |
| [#105](https://github.com/PyDevices/audiodsp/issues/105) | `audiodynamics.Dynamics` on an ESP32 | the compressor's detector, from settings that are bit-identical |
| [#115](https://github.com/PyDevices/audiodsp/issues/115) | `Compressor` and `TransientShaper`, Windows against unix | single-precision libm, same C |
| [#55](https://github.com/PyDevices/audiodsp/issues/55) | `Dynamics` and `Filter`, board against desktop | a second cause beside FMA, never identified |

**The two widths are 24 bits of significand and 53.** The difference each one
produces is the same size every time: a sample or a few per render, each one or
two LSB, which is 90 dB or more below full scale and inaudible by a wide
margin. #101 and #102 are the two the gate actually sees, as
`biquad_component_probe.py` and `echo_filter_probe.py` on the
single-precision leg.

**Why it is left.** Fixing it means one of two things and neither is worth its
price. Deriving at `double` in the bindings puts a software double on every
board that has no hardware for one, in code that runs per block, to move a
value 90 dB down. Deriving at `float` everywhere makes the desktop targets
wrong instead of the boards and breaks every stored digest in the repository.
The third option -- a shared derivation helper at a fixed width, which is what
`audiodsp_biquad_cp_w0` already is for the CPython leg -- is the right answer
*for a node being written now*, and is how new shared code should do it; it is
not worth retrofitting through six call sites and the goldens they move.

**What the gate does instead.** `verify_dsp.py`'s `--known-divergent` takes a
bound: `PROBE:SAMPLES:LSB`. A listed probe may differ, by at most that many
samples and at most that many LSB, and the run fails if the divergence grows,
if a probe stops diverging, or if any other probe diverges at all. A bare
`PROBE` with no bound is an *exemption* rather than a tolerance; the gate
still accepts it, prints the size it measured and names the bound that should
replace it. That distinction is the whole point: an accepted difference that
nobody has measured is indistinguishable from a new defect.

## The one thing this page does not cover

Nothing establishes that a node of ours *sounds right*, or that its algorithm is
the one intended. Three-target agreement proves consistency, traits prove stated
properties. Neither proves a design. That comes from a dossier, a reference
recording, or Brad's ear, and it belongs to the effects program's gates rather
than to this page.
