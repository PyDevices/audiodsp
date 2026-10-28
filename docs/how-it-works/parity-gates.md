# Running the parity gates

CI runs the CPython fixture tests and the five parity gates whose goldens live
in this repository (`verify_acceptance`, `verify_effects`, `verify_streaming`,
`verify_biquad`, `verify_mixdown_knee`; see [Contributing](https://github.com/PyDevices/audiodsp/blob/main/CONTRIBUTING.md#test)).
The deeper gates need built interpreters and the pinned CircuitPython oracle,
so CI doesn't run them. This page is for when you do. What each module is held
to is in [the correctness standard](correctness-standard.md).

## The oracle binary

CircuitPython at the tag in `CIRCUITPYTHON_ORACLE` is the oracle every
CircuitPython-shared parity golden is measured against. Build it as
CircuitPython's unix coverage variant with `CIRCUITPY_SYNTHIO_MAX_CHANNELS=64`
and this repository as its only audio user C module (`circuitpython.mk`), at
the same version and voice ceiling this port ships. An oracle at another
configuration can't answer the only question worth asking. Re-pin its hash in
`tests/test_voice_ceiling_consistency.py` in the same change that builds it.

A stock `circuitpython` unix build is not the oracle: the coverage variant's own
ceiling is 14 voices, and it changes whenever the interpreter is rebuilt.

Never edit CircuitPython's own sources for the oracle. A modified oracle
redefines what parity means and invalidates every golden without failing
anything. The pin moves only when the port moves to a new CircuitPython
release, in a change that re-reads every CircuitPython-shared gate and writes
the reason down. Every pin move is a resync: take every audio fix and addition
upstream made since the old pin, following [upstream-sync](upstream-sync.md).

Extending CircuitPython is the designed path, and it is additive only. New
modules live under `src/circuitpython_spike/` and build with `src/shared/` as a
user C module. Never make one shadow or rewrite a CircuitPython module, because
the oracle is built with it and a behavioral change would leak into the oracle.
After any CircuitPython-adjacent work, check that `circuitpython.mk` compiles
only this repository's modules and `src/shared/`, never a file under
CircuitPython's `shared-module/` or `shared-bindings/` for `synthio`,
`audiofilters`, `audiocore`, `audiomixer` or `audiodelays`.

## Running them

Every script under `tests/parity/` runs unchanged against this port and the
oracle, renders PCM and diffs it byte for byte, or documents the exact bounded
exception in [upstream-diff](upstream-diff.md). The scripts' zero-argument
defaults don't point at any directory, so pass `--micropython` and
`--circuitpython`, or set `CP_DIR`, `MP_UNIX` or `ULAB_DIR`.

The nine modules that are ours have no oracle
([the standard](correctness-standard.md)). They are held to two things: every
target renders them identically, and their numeric traits hold.

```sh
python tests/parity/verify_dsp.py --micropython <path to the MicroPython unix binary>
```

Add `--circuitpython <oracle>` for the three-way. There is no stored digest: the
gate is the comparison, and it refuses a run with fewer than two interpreters,
because one interpreter can't disagree with itself. The arithmetic is all in
`src/shared/`, so two interpreters disagreeing is never a difference of intent;
it is a width, an undefined shift, a compiler's choice or an architecture.

The traits are the other half, one file per module:
`tests/test_cpython_<module>.py`, each opening with its trait table and the bar
for every row. Agreement can't see a change that moves all three targets
together; the traits can, because they are measured against arithmetic rather
than against our own last answer.

`verify_mixdown_knee` is the only gate whose material crosses the mix-down
limiter's +/-28000 knee, so it is the only one that can see a synthio voice
ceiling change. It sees one of the ceiling's five sites, `max_polyphony` in
`src/cpython/synthio.py`, because that is the only one the CPython target reads;
`tests/test_voice_ceiling_consistency.py` guards against a half-applied change.
Its above-knee half is enforced against this port, not the oracle: the port is
built at 64 voices and the pinned oracle at the same ceiling it ships.

## Two kinds of golden

The instruments digests (`instruments_*.json`, under
[audiocomponents](https://github.com/PyDevices/audiocomponents)'
`tests/parity/golden/`) record that the port matches the pre-rewrite original
script within one interpreter. They never consult the oracle. So a correct
change to `src/cpython/` here can still stale those CPython digests.

A CPython-target fix may re-capture the affected digests only if it carries
independent evidence against the built oracle: a test in
`tests/test_cpython_*.py` run against the oracle and cited in the commit
message. The audiocomponents re-capture names the commit here that carries that
evidence. A fix that merely asserts oracle intent doesn't qualify, since that
would let it rewrite its own reference. The stored digest is the only thing in
that gate that notices the engine moving under both the original and the port,
so it stays. Each alarm costs one adjudication; that is the price of the signal.
