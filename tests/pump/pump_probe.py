"""The pump's own gates: same bytes, no allocation, survives a storm.

    <interpreter> tests/pump/pump_probe.py [case ...] [--fault WHICH]

Cases: ``identity``, ``alloc``, ``storm``, ``driver`` (all by default).
Faults: ``short`` and ``reuse`` (identity), ``alloc`` (alloc),
``stall`` (storm). Each one must make this exit non-zero; a probe whose
failing mode is never run is not a gate.

Needs a MicroPython build carrying this repository as a usermod -- the one
``clean-build.yml`` makes. It does **not** need the platform driver: without
one ``audiopump.threaded()`` is False and the same loop runs on the
interpreter's own thread, which is what the ``driver`` case checks.

What each case is for
---------------------

``identity``  The pump's claim. Five graphs built from this repository's own
    nodes, each pulled twice: once by ``audiopump.pull()`` on this thread,
    once by ``audiopump.spawn()`` on the pump's, while the interpreter is
    deliberately busy with a GC storm. The digest is FNV-1a 64 over every
    byte, computed in C (``AUDIOPUMP_STATUS_DIGEST``) -- so the comparison
    is of the audio, not of a Python copy of it. **The graph is rebuilt
    between the two runs**, because a delay line, a reverb tank and an
    envelope all carry state: the second pull of one graph is not the same
    audio as the first, and comparing those would be comparing two different
    things and calling it a difference. ``--fault reuse`` does exactly that,
    to show the difference is visible.

``alloc``   ``micropython.heap_lock()`` turns any ``m_malloc`` into a raise,
    so a pull that survives it allocated nothing -- no averaging, no
    ``gc.mem_free()`` delta to read. Every graph is gated **cold**: built,
    then locked, with no warm-up pull, because a warm-up hides exactly the
    first-block allocation a pump hits on its first block. This is a small
    representative set of node types, not the whole palette; the palette is
    swept by ``docs/spikes/probes/alloc_gate.py`` in the workspace anchor,
    which needs the component tier this repository does not depend on.

``storm``   Retargeting the pump onto new graphs, and releasing the old ones,
    **without parking it** -- the shape that caught a real race on an
    ESP32-P4: a root deinited straight after the retarget while the pump may
    still have been inside it, which shows up as fault=2 and a dead pump.

``driver``  Which platform driver the engine bound, and whether that agrees
    with ``threaded()``. A build with no driver still pumps; it just runs the
    loop on this thread.
"""

import gc
import os
import struct
import sys

import micropython

import audiocore
import audiodelays
import audiofilters
import audiofreeverb
import audiomixer
import audiopump
import synthio
from array import array

RATE = 48000
CHANNELS = 2
BLOCK_FRAMES = 256

#: Blocks per identity run. Long enough for a reverb tank to fill and for
#: the GC storm to land inside the pump's run, short enough to be seconds.
BLOCKS = 4000

#: Blocks per cold allocation gate. The first is the one that matters.
ALLOC_BLOCKS = 60

WORDS = "<8Q"
BLOCKS_AT, BYTES_AT, DIGEST_AT, RESULT_AT, RUNNING_AT, ERROR_AT = range(6)
FAULT_AT = 24


def status():
    return bytearray(audiopump.STATUS_BYTES)


def read(block):
    return struct.unpack_from(WORDS, block)


def fault_word(block):
    return struct.unpack_from("<Q", block, FAULT_AT * 8)[0]


def tone(frames=BLOCK_FRAMES * 40, step=97):
    """A deterministic buffer -- no clock, no random, no float rounding."""
    data = array("h", bytes(2 * frames * CHANNELS))
    for index in range(frames):
        value = ((index * step) % 4096) - 2048
        for channel in range(CHANNELS):
            data[index * CHANNELS + channel] = value
    return data


def raw():
    return audiocore.RawSample(tone(), sample_rate=RATE,
                               channel_count=CHANNELS)


# --- the five graphs -------------------------------------------------------
#
# One per node family this repository owns, each holding the state that makes
# a second pull of the same object different from the first: an envelope, a
# delay line, a reverb tank, a filter's history, a mixer's voice position.


def graph_synth(keep):
    synth = synthio.Synthesizer(sample_rate=RATE, channel_count=CHANNELS)
    envelope = synthio.Envelope(attack_time=0.01, decay_time=0.1,
                                release_time=0.2, attack_level=0.8,
                                sustain_level=0.4)
    for frequency in (164.81, 246.94, 329.63):
        synth.press(synthio.Note(frequency=frequency, envelope=envelope))
    keep.append(synth)
    return synth


def graph_echo(keep):
    source = raw()
    echo = audiodelays.Echo(max_delay_ms=200, delay_ms=80, decay=0.6,
                            sample_rate=RATE, channel_count=CHANNELS,
                            buffer_size=BLOCK_FRAMES * CHANNELS * 2)
    echo.play(source, loop=True)
    keep += [source, echo]
    return echo


def graph_filter(keep):
    source = raw()
    node = audiofilters.Filter(
        filter=synthio.Biquad(synthio.FilterMode.LOW_PASS,
                              frequency=900.0),
        sample_rate=RATE, channel_count=CHANNELS,
        buffer_size=BLOCK_FRAMES * CHANNELS * 2)
    node.play(source, loop=True)
    keep += [source, node]
    return node


def graph_reverb(keep):
    source = raw()
    verb = audiofreeverb.Freeverb(roomsize=0.7, damp=0.4, mix=0.5,
                                  sample_rate=RATE, channel_count=CHANNELS,
                                  buffer_size=BLOCK_FRAMES * CHANNELS * 2)
    verb.play(source, loop=True)
    keep += [source, verb]
    return verb


def graph_mixer(keep):
    mixer = audiomixer.Mixer(voice_count=2, sample_rate=RATE,
                             channel_count=CHANNELS, bits_per_sample=16,
                             samples_signed=True,
                             buffer_size=BLOCK_FRAMES * CHANNELS * 2)
    for voice, step in enumerate((97, 61)):
        source = audiocore.RawSample(tone(step=step), sample_rate=RATE,
                                     channel_count=CHANNELS)
        keep.append(source)
        mixer.voice[voice].level = 0.5
        mixer.play(source, voice=voice, loop=True)
    keep.append(mixer)
    return mixer


GRAPHS = (("synth", graph_synth), ("echo", graph_echo),
          ("filter", graph_filter), ("reverb", graph_reverb),
          ("mixer", graph_mixer))


def say(name, ok, detail):
    print("  %-9s %-4s %s" % (name, "ok" if ok else "FAIL", detail))
    return ok


# --- identity --------------------------------------------------------------


def interpreter_pull(build, blocks):
    keep = []
    block = status()
    audiopump.pull(build(keep), blocks, block)
    return read(block), keep


def pump_pull(build, blocks, graph=None):
    """The same pull on the pump's thread, with this one deliberately busy.

    On a build with no platform driver there IS no other thread: `spawn()`
    leaves the loop in service mode and `service()` runs it here. The churn
    still happens between blocks, which is the same question asked of the
    shape that port has.
    """
    keep = []
    tail = build(keep) if graph is None else graph
    block = status()
    threaded = bool(audiopump.threaded())
    audiopump.spawn(tail, blocks, block)
    rounds = 0
    # Driven off the BLOCK COUNT, not off `running`: the pump's thread sets
    # `running` itself, so there is a window after spawn() where it still
    # reads 0 and a loop waiting on it exits before the storm has churned
    # once. Then the digests match because nothing ever happened.
    while read(block)[BLOCKS_AT] < blocks and rounds < 200000:
        if not threaded:
            audiopump.service(16)
        rubbish = [bytearray(64) for _ in range(200)]
        rubbish.clear()
        gc.collect()
        rounds += 1
    if rounds >= 200000:
        audiopump.stop()
    audiopump.join()
    audiopump.shutdown()
    return read(block), rounds, keep


def identity(fault):
    ok = True
    for name, build in GRAPHS:
        gc.collect()
        blocks = BLOCKS
        here, keep = interpreter_pull(build, blocks)
        if fault == "short":
            blocks = BLOCKS // 2
        there, rounds, other = pump_pull(
            build, blocks, graph=keep[-1] if fault == "reuse" else None)
        same = (here[DIGEST_AT] == there[DIGEST_AT]
                and here[BLOCKS_AT] == there[BLOCKS_AT]
                and here[BLOCKS_AT] > 0)
        ok = say(name, same,
                 "interp %016x / pump %016x over %d/%d blocks, %d gc rounds "
                 "on the interpreter" % (here[DIGEST_AT], there[DIGEST_AT],
                                         here[BLOCKS_AT], there[BLOCKS_AT],
                                         rounds)) and ok
        del keep, other
    return ok


# --- the cold allocation gate ----------------------------------------------


def gate(tail, blocks=ALLOC_BLOCKS, allocate=False):
    block = status()
    gc.collect()
    raised = None
    micropython.heap_lock()
    try:
        if allocate:
            # Not a pull: the gate's own mechanism, shown firing. If this
            # does NOT raise, heap_lock is doing nothing and every "ok"
            # below is worthless.
            if len(bytearray(64)) != 64:
                raise AssertionError("unreachable")
        audiopump.pull(tail, blocks, block)
    except BaseException as exc:          # noqa: BLE001 - the gate's point
        raised = exc
    micropython.heap_unlock()
    return raised, read(block)


def alloc(fault):
    ok = True
    for name, build in GRAPHS:
        keep = []
        tail = build(keep)
        raised, block = gate(tail, allocate=(fault == "alloc"))
        clean = raised is None and block[ERROR_AT] in (0, 3)
        detail = ("%d blocks pulled with the heap locked"
                  % block[BLOCKS_AT])
        if raised is not None:
            detail = ("allocated after %d blocks: %s: %s"
                      % (block[BLOCKS_AT], type(raised).__name__, raised))
        elif block[ERROR_AT] not in (0, 3):
            detail = "the pull stopped with error %d" % block[ERROR_AT]
        ok = say(name, clean, detail) and ok
        del keep
    return ok


# --- the storm -------------------------------------------------------------


def wait_for(block, blocks, spins=200000):
    """Wait until the pump has pulled `blocks` more. True if it did.

    Where there is no driver the pump has no thread, so waiting for it to
    get on with it would wait for ever: `service()` is the loop, on this
    thread, and the wait is a call rather than a spin.
    """
    want = read(block)[BLOCKS_AT] + blocks
    threaded = bool(audiopump.threaded())
    spun = 0
    while read(block)[BLOCKS_AT] < want and spun < spins:
        if not threaded:
            audiopump.service(blocks)
        spun += 1
    return read(block)[BLOCKS_AT] >= want


def storm(fault):
    """Retarget onto a new graph and release the old one, never parking.

    The release is a real ``deinit()``, not a dropped reference: dropping one
    leaves the node alive until a collection that may never come, so it would
    prove nothing. What must hold is that the pump is demonstrably **inside
    the new graph** before the old one goes -- that is the wait below, and
    `--fault stall` deinits the graph the pump is still pulling to show the
    probe can see the fault that follows.
    """
    rounds = 12
    keep = []
    current = graph_mixer(keep)
    block = status()
    audiopump.spawn(current, 0x7FFFFFFF, block)
    moved = 0
    stalled = 0
    if fault == "stall":
        wait_for(block, 4)
        current.deinit()                  # released under a running pump
        wait_for(block, 4)
    else:
        for _round in range(rounds):
            fresh = []
            nxt = graph_echo(fresh)
            audiopump.retarget(nxt)
            if not wait_for(block, 2):
                stalled += 1
            current.deinit()              # the old root, one graph late
            del keep[:]
            keep = fresh
            current = nxt
            moved += 1
    audiopump.stop()
    audiopump.join()
    audiopump.shutdown()
    final = read(block)
    clean = final[ERROR_AT] == 0 and fault_word(block) == 0
    return say("storm", clean and not stalled and final[BLOCKS_AT] > 0,
               "%d retargets, %d stalls, %d blocks, error=%d fault=%d"
               % (moved, stalled, final[BLOCKS_AT], final[ERROR_AT],
                  fault_word(block)))


# --- the driver ------------------------------------------------------------


def driver(fault):
    name = audiopump.driver()
    threads = bool(audiopump.threaded())
    # A build with no driver bound still pumps: `threaded()` goes False and
    # `service()` runs the same loop on the interpreter's thread. A build
    # WITH one has a thread. Those are the only two shapes.
    agrees = (name == "none") != threads
    known = name in ("none", "pthread", "win32", "esp32")
    return say("driver", agrees and known,
               "driver()=%r threaded()=%s" % (name, threads))


# --- unpumpable ------------------------------------------------------------


#: Same convention route_probe.py uses: CI points this at the runner's temp
#: directory so a probe never writes into the checkout.
_TMP = os.getenv("PUMP_PROBE_TMP", "/tmp")


def _wave_file(path=None, frames=2048):
    """A real RIFF/WAVE on the filesystem, because `WaveFile` parses one."""
    path = path or (_TMP + "/audiodsp_probe.wav")
    data = bytearray()
    for frame in range(frames):
        value = (frame * 97) % 20000 - 10000
        data += bytes((value & 0xFF, (value >> 8) & 0xFF))
    header = (b"RIFF" + (36 + len(data)).to_bytes(4, "little") + b"WAVEfmt "
              + (16).to_bytes(4, "little") + (1).to_bytes(2, "little")
              + (1).to_bytes(2, "little") + (8000).to_bytes(4, "little")
              + (16000).to_bytes(4, "little") + (2).to_bytes(2, "little")
              + (16).to_bytes(2, "little")
              + b"data" + len(data).to_bytes(4, "little"))
    with open(path, "wb") as handle:
        handle.write(header + bytes(data))
    return path


def _refused(tail):
    """Did `spawn()` say no, with the sentence rather than a fault number?"""
    block = status()
    try:
        # Positional, as everywhere else in this file: spawn's first three
        # arguments are required and the binding wants them that way.
        audiopump.spawn(tail, 4, block)
    except ValueError as error:
        return "file-backed" in str(error)
    audiopump.shutdown()
    return False


def unpumpable(fault):
    """audiodsp#112. A file-backed source is refused at HANDOVER, however deep
    in the graph it sits -- not found by the pump at the first block, as a
    fault and silence.

    The depth is the whole point: a `WaveFile` as the tail was always caught,
    because the old check read one type name. Behind a Filter, or behind a
    Mixer's second voice, it was not.

    `--fault unpumpable` asserts the *negative* control instead: a graph with
    no file in it must NOT be refused, so a check that simply raised every
    time would fail here.
    """
    import audiocore
    import audiomixer
    import audioroute

    path = _wave_file()
    keep = []
    ok = True

    if fault == "unpumpable":
        # The control: nothing file-backed anywhere, so spawn() must accept.
        tail = raw()
        refused = _refused(tail)
        if not refused:
            audiopump.shutdown()
        return say("unpumpable", refused,
                   "a clean graph was accepted, so the check does not just "
                   "raise -- inverted by --fault, and this must FAIL")

    wave = audiocore.WaveFile(open(path, "rb"))
    keep.append(wave)
    ok = say("tail", _refused(wave), "a WaveFile handed over directly") and ok

    # A Port rather than a Filter: it takes its source positionally and
    # adopts its format, so the row is about the WALK and not about matching
    # a rate.
    wave = audiocore.WaveFile(open(path, "rb"))
    deep = audioroute.Port(wave)
    keep.extend((wave, deep))
    ok = say("one deep", _refused(deep), "a WaveFile behind a Port") and ok

    wave = audiocore.WaveFile(open(path, "rb"))
    mixer = audiomixer.Mixer(voice_count=2, sample_rate=8000, channel_count=1,
                             bits_per_sample=16, samples_signed=True,
                             buffer_size=512)
    # Voice 0 deliberately left silent: an empty slot must not end the walk.
    mixer.play(wave, voice=1)
    keep.extend((wave, mixer))
    ok = say("behind mix", _refused(mixer),
             "a WaveFile on a Mixer's SECOND voice, first voice silent") and ok

    ok = say("clean", not _refused(raw()),
             "a graph with no file in it is still accepted") and ok
    return ok


CASES = (("identity", identity), ("alloc", alloc), ("storm", storm),
         ("driver", driver), ("unpumpable", unpumpable))


def main():
    args = sys.argv[1:]
    fault = None
    if "--fault" in args:
        index = args.index("--fault")
        fault = args[index + 1]
        del args[index:index + 2]
    wanted = args or [name for name, _run in CASES]

    print("audiodsp: the pump's own gates    fault: %s" % fault)
    ok = True
    for name, run in CASES:
        if name not in wanted:
            continue
        print(" %s" % name)
        ok = run(fault) and ok
    print("VERDICT:", "kept" if ok else "BROKEN")
    return 0 if ok else 1


sys.exit(main())
