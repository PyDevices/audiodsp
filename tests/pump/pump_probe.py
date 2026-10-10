"""The pump's own gates: same bytes, no allocation, survives a storm.

    <interpreter> tests/pump/pump_probe.py [case ...] [--fault WHICH]

Cases: ``identity``, ``alloc``, ``storm``, ``driver``, ``unpumpable``,
``swap``, ``writes``, ``tear``, ``fade`` (all by default).
Faults: ``short`` and ``reuse`` (identity), ``alloc`` (alloc),
``stall`` (storm), ``unpumpable`` (unpumpable), ``unlocked`` (swap, writes),
``split`` (tear), ``cut`` (fade). Each one must make this exit non-zero; a probe whose
failing mode is never run is not a gate.

Runs on a MicroPython build carrying this repository as a usermod -- the one
``clean-build.yml`` makes -- and on CPython with this repository's wheel
installed. It does **not** need the platform driver: without one
``audiopump.threaded()`` is False and the same loop runs on the
interpreter's own thread, which is what the ``driver`` case checks and what
CPython always does. ``alloc`` needs MicroPython's heap lock and says it was
skipped anywhere else.

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
    representative set of node types, not the whole palette; the full palette
    needs the component tier, which this repository does not depend on.

``storm``   Retargeting the pump onto new graphs, and releasing the old ones,
    **without parking it** -- the shape that caught a real race on an
    ESP32-P4: a root deinited straight after the retarget while the pump may
    still have been inside it, which shows up as fault=2 and a dead pump.

``driver``  Which platform driver the engine bound, and whether that agrees
    with ``threaded()``. A build with no driver still pumps; it just runs the
    loop on this thread.

``swap``    ``Convolver.load()`` and ``Convolver.synthesize()`` on a node the
    pump is pulling. Both used to build the new impulse in place with no lock,
    and the pull's own transform shares the scratch the build writes each
    partition through, so a pull that landed mid-build left that partition
    wrong until the next load (audiodsp#166). Where the pump has a thread of
    its own, the room is rebuilt over and over while the pump pulls it, and
    must then render exactly as a room built in peace does. Everywhere, the
    lock ledger must show the call took the lock, and where it can time the
    hold, the hold must be well under the call: the build runs unlocked and
    only the install is held. ``--fault unlocked`` counts across a call that
    takes no lock, and must fail. Skipped on CPython, whose extension has no
    pump lock: nothing there pulls from another thread.

``writes``  The control writes a pull reads, on the delays: every
    ``FeedbackDelay.set()`` option, and ``MultiTapDelay``'s ``taps`` and
    ``delay_ms``. Each must take the pump lock. ``taps`` used to resize its
    tables in place, freeing them while a pull on the pump's thread could be
    reading them, and the rest wrote what a pull reads with no lock at all
    (audiodsp#177). ``--fault unlocked`` writes ``mix`` instead, which takes
    no lock, and must fail. Skipped on CPython, as ``swap`` is.

``tear``    One ``set()`` is one change. The six nodes whose ``set()``
    takes a table of options -- ``Waveshaper``, ``Ladder``, ``Bank``,
    ``Dynamics``, ``FeedbackDelay`` and ``Tank`` -- used to write each option
    into the running config as it was read, so a pull landing between two
    options of one call played a block of a filter nobody asked for: a click
    (audiodsp#109). The pull is put exactly there, on every build: the second
    option's value is an object whose ``__float__`` pulls a block from the
    node, which is the moment the pump's thread would land. That block must
    be the one a twin with the same history renders with the whole OLD
    setting. Each class first shows the first option alone changes the block,
    so a match is not a comparison the material cannot fail. A ``set()`` that
    raises on a bad keyword must change nothing at all. Where the pump has a
    thread, a stateless ``Waveshaper`` is then flipped between two settings
    while the pump pulls it, and every sample must belong to one setting or
    the other. ``--fault split`` applies the first option in a call of its
    own -- the shape of the old code -- and must fail. Skipped on CPython,
    whose extension has no pump.

``fade``    A swap that outlasts the ring fades out and back in instead of
    cutting the speaker off. ``retarget(tail, fade=True)`` between two
    constant tails must put out the old level ramped to silence over one
    block and then the new level ramped up from it over the next -- exact
    values, on every build, through ``service()`` where there is no thread.
    Where there is a thread, ``park(fade=True)`` is held for 200 ms while
    this thread drains the ring at the speaker's pace: the ring must never
    run dry, the audio must ramp down, stay silent and ramp back up, and a
    park that ends before the deadline must be inaudible -- the same audio
    as no park at all. ``--fault cut`` asks for neither fade, and must fail.
"""

import gc
import os
import struct
import sys

try:
    import micropython
except ImportError:  # CPython: no heap lock, so no allocation gate
    micropython = None

import audioconvolve
import audiocore
import audiodelays
import audioecho
import audiofilters
import audiofreeverb
import audiodynamics
import audiomixer
import audiomodal
import audiopump
import audioladder
import audioshaper
import audioverb
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
    if micropython is None:
        # CPython allocates for every Python object it touches and has no
        # heap lock to prove otherwise, so this gate is MicroPython's alone,
        # and so is its planted fault. Said rather than silently passed.
        print("  alloc     skip no heap lock on this interpreter")
        return True
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
_TMP = os.getenv("PUMP_PROBE_TMP")
if _TMP is None:
    try:
        import tempfile
        _TMP = tempfile.gettempdir()
    except ImportError:      # MicroPython
        _TMP = "/tmp"


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


# --- swap ------------------------------------------------------------------


def _ticks_us():
    import time
    if hasattr(time, "ticks_us"):
        return time.ticks_us()
    return int(time.perf_counter() * 1000000)


def _room():
    return audioconvolve.Convolver(sample_rate=RATE, channel_count=CHANNELS,
                                   max_taps=3840, ir_channels=2)


def _impulse(seed):
    taps = array("h", [0] * (3840 * 2))
    value = seed
    for i in range(len(taps)):
        value = (value * 1103515245 + 12345) & 0x7FFFFFFF
        taps[i] = ((value >> 16) & 0x3FFF) - 0x2000
    return taps


def _render(node, blocks=40):
    node.clear()
    node.play(raw())
    out = bytearray()
    for _ in range(blocks):
        out += bytes(audiocore.get_buffer(node)[1])
    return bytes(out)


def _rebuilt_under_the_pump(rebuild, finish):
    """Rebuild a room the pump is pulling, then compare it with one built
    while nothing pulls. Returns (same, blocks the pump pulled)."""
    room = _room()
    room.play(raw())
    room.synthesize(decay=0.08, seed=1)
    block = status()
    audiopump.spawn(room, 1 << 30, block)
    spins = 0
    while read(block)[BLOCKS_AT] < 50 and spins < 1000000:
        spins += 1
    for round_ in range(30):
        rebuild(room, round_)
    finish(room)
    pulled = read(block)[BLOCKS_AT]
    audiopump.stop()
    audiopump.join()
    audiopump.shutdown()
    reference = _room()
    finish(reference)
    same = _render(room) == _render(reference)
    room.deinit()
    reference.deinit()
    return same, pulled


def swap(fault):
    if sys.implementation.name == "cpython":
        return say("swap", True, "skipped: the CPython extension has no pump "
                   "lock, and nothing there pulls from another thread")
    threaded = bool(audiopump.threaded())
    ok = True
    if threaded:
        impulses = (_impulse(3), _impulse(4))
        races = (
            ("synthesize",
             lambda room, k: room.synthesize(decay=0.08, seed=7 + k % 2),
             lambda room: room.synthesize(decay=0.08, seed=7)),
            ("load",
             lambda room, k: room.load(impulses[k % 2], channels=2),
             lambda room: room.load(impulses[0], channels=2)),
        )
        for name, rebuild, finish in races:
            same, pulled = _rebuilt_under_the_pump(rebuild, finish)
            ok = say(name, same, "%s after 30 rebuilds while the pump pulled "
                     "%d blocks" % ("the same room" if same else
                                    "a DIFFERENT room", pulled)) and ok
    room = _room()
    room.play(raw())
    room.synthesize(decay=0.08, seed=1)
    for _ in range(4):
        audiocore.get_buffer(room)
    impulse = _impulse(5)
    calls = (
        ("synthesize", lambda: room.synthesize(decay=0.08, seed=2)),
        ("load", lambda: room.load(impulse, channels=2)),
    )
    for name, call in calls:
        if fault == "unlocked":
            call = lambda: room.set(mix=0.5)   # noqa: E731 -- takes no lock
        gc.collect()
        audiopump.lock_reset()
        start = _ticks_us()
        call()
        took = _ticks_us() - start
        stats = audiopump.lock_stats()
        takes, held = stats[3], stats[6]
        good = takes >= 1
        detail = "%d lock take(s)" % takes
        if threaded:
            # The build is the long part and runs unlocked; what the lock
            # covers is a copy and two one-block convolutions per channel.
            good = good and held * 2 < took
            detail += ", held %d us of a %d us call" % (held, took)
        ok = say(name, good, detail) and ok
    room.deinit()
    return ok


# --- writes ----------------------------------------------------------------


def writes(fault):
    if sys.implementation.name == "cpython":
        return say("writes", True, "skipped: the CPython extension has no "
                   "pump lock, and nothing there pulls from another thread")
    echo = audioecho.FeedbackDelay(max_delay_ms=200, sample_rate=RATE,
                                   channel_count=CHANNELS)
    taps = audiodelays.MultiTapDelay(
        max_delay_ms=200, delay_ms=150, taps=(0.2, 0.5), sample_rate=RATE,
        channel_count=CHANNELS, buffer_size=BLOCK_FRAMES * 4)
    for node in (echo, taps):
        node.play(raw())
        for _ in range(4):
            audiocore.get_buffer(node)
    shape = array("h", [0, 16000, 0, -16000])
    calls = [
        ("echo.set(%s)" % name,
         lambda name=name: echo.set(**{name: 0.25}))
        for name in ("delay_ms", "feedback", "mix", "damping_hz", "cut_hz",
                     "wow_hz", "wow_depth_ms", "cross_feed", "loop_drive",
                     "input_pan", "delay_slew", "wow_am_depth",
                     "loop_semitones", "loop_window_ms")
    ]
    calls.append(("echo.set(wow_shape)", lambda: echo.set(wow_shape=shape)))
    calls.append(("taps.taps", lambda: setattr(taps, "taps", (0.1, 0.4, 0.7))))
    calls.append(("taps.delay_ms", lambda: setattr(taps, "delay_ms", 90)))
    ok = True
    for name, call in calls:
        if fault == "unlocked":
            call = lambda: setattr(taps, "mix", 0.5)   # noqa: E731
        audiopump.lock_reset()
        call()
        takes = audiopump.lock_stats()[3]
        ok = say(name, takes >= 1, "%d lock take(s)" % takes) and ok
    echo.deinit()
    taps.deinit()
    return ok


# --- tear ------------------------------------------------------------------


class _Midway:
    """A value whose float conversion pulls a block from `node` first -- the
    moment a pull on the pump's thread lands in the middle of a set()."""

    def __init__(self, node, value):
        self.node = node
        self.value = value
        self.block = None

    def __float__(self):
        self.block = bytes(audiocore.get_buffer(self.node)[1])
        return self.value


_LINEAR = array("h", [-32767, 32767])


def _tear_nodes():
    """(name, build, set): building twice gives two nodes with identical
    history. `set(node, **extra)` calls set() with a first option and then
    `extra`, as literal keywords, because a keyword call keeps its order and a
    `**dict` does not: MicroPython's dict is unordered, and a probe that splat
    one put the hook ahead of the option it was meant to follow on five of six
    classes, and passed the unfixed code."""
    def shaper():
        return audioshaper.Waveshaper(curve=_LINEAR, sample_rate=RATE,
                                      channel_count=CHANNELS)

    def ladder():
        return audioladder.Ladder(sample_rate=RATE, channel_count=CHANNELS,
                                  cutoff_hz=2000)

    def bank():
        node = audiomodal.Bank(modes=2, sample_rate=RATE,
                               channel_count=CHANNELS, mix=0.5)
        node.set_mode(0, 220.0, 0.5, 1.0)
        node.set_mode(1, 660.0, 0.3, 0.5)
        return node

    def dynamics():
        return audiodynamics.Dynamics(sample_rate=RATE, channel_count=CHANNELS,
                                      threshold_db=-40, ratio=2)

    def echo():
        return audioecho.FeedbackDelay(max_delay_ms=20, delay_ms=5,
                                       sample_rate=RATE,
                                       channel_count=CHANNELS)

    def tank():
        return audioverb.Tank(sample_rate=RATE, channel_count=CHANNELS)

    return (
        ("Waveshaper", shaper,
         lambda n, **k: n.set(pre_gain=2.0, **k), "post_gain", 0.5),
        ("Ladder", ladder,
         lambda n, **k: n.set(cutoff_hz=300.0, **k), "resonance", 1.5),
        ("Bank", bank, lambda n, **k: n.set(mix=1.0, **k), "gain", 2.0),
        ("Dynamics", dynamics,
         lambda n, **k: n.set(threshold_db=-30.0, **k), "ratio", 8.0),
        ("FeedbackDelay", echo,
         lambda n, **k: n.set(mix=0.9, **k), "feedback", 0.6),
        ("Tank", tank, lambda n, **k: n.set(mix=0.8, **k), "decay", 0.3),
    )


def _twins(build):
    nodes = []
    for _ in range(2):
        node = build()
        node.play(raw())
        for _ in range(6):
            audiocore.get_buffer(node)
        nodes.append(node)
    return nodes


def _tear_race(fault, rounds=4000):
    """A stateless Waveshaper flipped between two settings while the pump
    pulls it: every output sample belongs to one setting or to the other."""
    level = 8000
    flat = array("h", [level] * (BLOCK_FRAMES * CHANNELS))
    a = {"pre_gain": 1.0, "post_gain": 1.0}
    b = {"pre_gain": 0.5, "post_gain": 0.5}

    def node_at(setting):
        node = audioshaper.Waveshaper(curve=_LINEAR, oversample=1,
                                      sample_rate=RATE,
                                      channel_count=CHANNELS, **setting)
        node.play(audiocore.RawSample(flat, sample_rate=RATE,
                                      channel_count=CHANNELS))
        return node

    def value_at(setting):
        node = node_at(setting)
        block = array("h", bytes(audiocore.get_buffer(node)[1]))
        node.deinit()
        return set(block)

    good = value_at(a) | value_at(b)
    node = node_at(a)
    ring = bytearray(BLOCK_FRAMES * CHANNELS * 2 * 64)
    out = bytearray(len(ring))
    block = status()
    audiopump.spawn(node, 1 << 30, block, ring=ring)
    torn = 0
    seen = 0
    for round_ in range(rounds):
        if fault == "split":
            node.set(pre_gain=b["pre_gain"] if round_ % 2 else a["pre_gain"])
            node.set(post_gain=b["post_gain"] if round_ % 2 else a["post_gain"])
        else:
            node.set(**(b if round_ % 2 else a))
        if round_ % 16 == 15:
            got = audiopump.drain(out)
            samples = array("h", out[:got - got % 2])
            seen += len(samples)
            for sample in samples:
                if sample not in good:
                    torn += 1
    audiopump.stop()
    audiopump.join()
    audiopump.shutdown()
    node.deinit()
    return torn, seen


def tear(fault):
    if sys.implementation.name == "cpython":
        return say("tear", True, "skipped: the CPython extension has no pump, "
                   "and nothing there pulls between two options")
    ok = True
    for name, build, first, second, value in _tear_nodes():
        node, twin = _twins(build)
        old = bytes(audiocore.get_buffer(twin)[1])
        # Can the material see a half-applied set? A twin with only the first
        # option changed must render something else.
        probe, spare = _twins(build)
        first(probe)
        expressive = bytes(audiocore.get_buffer(probe)[1]) != old
        midway = _Midway(node, value)
        # One keyword left in a **k is still one keyword, in its place after
        # the first option: the order that matters is first, then the hook.
        if fault == "split":
            first(node)
            node.set(**{second: midway})
        else:
            first(node, **{second: midway})
        whole = midway.block == old
        ok = say(name, expressive and whole,
                 "%s; %s" % ("a pull in the middle of set() heard the old "
                             "setting whole" if whole else
                             "a pull in the middle of set() heard HALF of it",
                             "the first option alone moves the block"
                             if expressive else
                             "the first option alone changes NOTHING, so this "
                             "cannot see a tear")) and ok
        # A refused set() leaves the node exactly as it was.
        for each in (node, twin):
            each.deinit()
        node, twin = _twins(build)
        refused = False
        try:
            if fault == "split":
                first(node)
                node.set(no_such_option=1.0)
            else:
                first(node, no_such_option=1.0)
        except TypeError:
            refused = True
        same = bytes(audiocore.get_buffer(node)[1]) == \
            bytes(audiocore.get_buffer(twin)[1])
        ok = say(name, refused and same,
                 "a refused set() %s" % ("changed nothing" if same else
                                         "LEFT PART OF ITSELF APPLIED")) and ok
        for each in (node, twin, probe, spare):
            each.deinit()
    if audiopump.threaded():
        torn, seen = _tear_race(fault)
        ok = say("race", torn == 0 and seen > 0,
                 "%d of %d samples from neither setting, a Waveshaper flipped "
                 "4000 times while the pump pulled it" % (torn, seen)) and ok
    return ok


# --- fade ------------------------------------------------------------------


def _ramp(level, frames, up):
    """What the pump's ramp makes of a constant `level` over one block."""
    out = []
    for k in range(frames):
        gain = ((k + 1) << 15) // frames if up else \
            (32768 * (frames - 1 - k)) // frames
        out.append((level * gain) >> 15)
    return out


def _flat(level, frames=BLOCK_FRAMES):
    return audiocore.RawSample(array("h", [level] * (frames * CHANNELS)),
                               sample_rate=RATE, channel_count=CHANNELS)


def _frames(data):
    samples = array("h", bytes(data))
    return [samples[i] for i in range(0, len(samples), CHANNELS)]


def _sleep_us(us):
    import time
    if hasattr(time, "sleep_us"):
        time.sleep_us(us)
    else:
        time.sleep(us / 1000000)


def _fade_swap(fault):
    """retarget(fade=True) between two constant tails, read from the ring."""
    old, new = _flat(8000), _flat(-6000)
    block = status()
    ring = bytearray(BLOCK_FRAMES * CHANNELS * 2 * 16)
    out = bytearray(len(ring))
    threaded = audiopump.spawn(old, 1 << 30, block, ring=ring, loop=True) >= 0
    got = bytearray()

    def take():
        if not threaded:
            audiopump.service(4)
        else:
            _sleep_us(2000)
        n = audiopump.drain(out)
        got.extend(out[:n])

    while len(got) < BLOCK_FRAMES * CHANNELS * 2 * 4:
        take()
    if fault == "cut":
        audiopump.retarget(new, loop=True)
    else:
        audiopump.retarget(new, loop=True, fade=True)
    while len(got) < BLOCK_FRAMES * CHANNELS * 2 * 40:
        take()
    fades = audiopump.fades()[0]
    audiopump.stop()
    audiopump.join()
    audiopump.shutdown()
    frames = _frames(got)
    # Where the swap landed: the first frame that is not the old level.
    at = 0
    while at < len(frames) and frames[at] == 8000:
        at += 1
    want = _ramp(8000, BLOCK_FRAMES, False) + _ramp(-6000, BLOCK_FRAMES, True)
    seen = frames[at:at + len(want)]
    tail_ok = all(f == -6000 for f in frames[at + len(want):])
    good = seen == want and tail_ok
    return say("swap", good, "%s at frame %d, %d fade(s)" % (
        "old ramped out over one block, new ramped in over the next"
        if good else "NOT the two ramps -- the swap cut", at, fades))


def _fade_park(fault, hold_ms):
    """Hold a park for `hold_ms` while this thread drains like a speaker.
    Returns (frames heard, times the speaker found the ring dry, fades())."""
    source = _flat(8000)
    block = status()
    ring = bytearray(BLOCK_FRAMES * CHANNELS * 2 * 8)
    piece = bytearray(BLOCK_FRAMES * CHANNELS * 2)
    audiopump.spawn(source, 1 << 30, block, ring=ring, loop=True)
    heard = bytearray()
    dry = [0]
    period = BLOCK_FRAMES * 1000000 // RATE

    def speaker():
        n = audiopump.drain(piece)
        if n < len(piece):
            dry[0] += 1
        heard.extend(piece[:n])
        _sleep_us(period)

    _sleep_us(20000)            # let the pump fill the ring, then play
    for _ in range(20):
        speaker()
    audiopump.park(fade=(fault != "cut"))
    for _ in range(hold_ms * 1000 // period):
        speaker()
    audiopump.unpark()
    for _ in range(30):
        speaker()
    stats = audiopump.fades()
    audiopump.stop()
    audiopump.join()
    audiopump.shutdown()
    return _frames(heard), dry[0], stats


def fade(fault):
    if sys.implementation.name == "cpython":
        return say("fade", True, "skipped here: the CPython twin has its own "
                   "test of the same ramps")
    ok = _fade_swap(fault)
    if not audiopump.threaded():
        return say("park", True, "skipped: a park with a deadline needs the "
                   "pump on a thread of its own") and ok
    frames, dry, stats = _fade_park(fault, 200)
    run = longest = 0
    for f in frames:
        run = run + 1 if f == 0 else 0
        longest = max(longest, run)
    start = 0
    while start < len(frames) and frames[start] == 8000:
        start += 1
    down = frames[start:start + BLOCK_FRAMES]
    after = start + BLOCK_FRAMES - 1
    while after < len(frames) and frames[after] == 0:
        after += 1
    up = frames[after:after + BLOCK_FRAMES]
    down_ok = down == _ramp(8000, BLOCK_FRAMES, False)
    up_ok = up == _ramp(8000, BLOCK_FRAMES, True)
    good = dry == 0 and down_ok and up_ok and longest * 1000 // RATE >= 100
    ok = say("park", good, "200 ms held: the speaker found the ring dry %d "
             "time(s); ramp down %s, %d ms of silence, ramp up %s; fades() %s"
             % (dry, "ok" if down_ok else "MISSING", longest * 1000 // RATE,
                "ok" if up_ok else "MISSING", stats)) and ok
    frames, dry, stats = _fade_park(fault, 5)
    clean = dry == 0 and all(f == 8000 for f in frames)
    ok = say("short", clean and stats[1] == 1,
             "5 ms held, inside the deadline: %s; clean holds %d" % (
                 "the same audio as no park at all" if clean else
                 "AUDIBLE", stats[1])) and ok
    return ok


CASES = (("identity", identity), ("alloc", alloc), ("storm", storm),
         ("driver", driver), ("unpumpable", unpumpable), ("swap", swap),
         ("writes", writes), ("tear", tear), ("fade", fade))


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
