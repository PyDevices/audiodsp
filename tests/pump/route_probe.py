"""The three things around the pump: its ring, its queue, its tap -- and
the port that must fault instead of recursing.

    <interpreter> tests/pump/route_probe.py [case ...] [--fault WHICH]

Cases: ``ring``, ``events``, ``tap``, ``port`` (all by default).
Faults: ``flip`` (ring), ``drop`` (events), ``short`` (tap), ``latch``
(port). Each must make this exit non-zero.

Everything here is measured on bytes, not on a description of bytes: the
ring case compares what came out of the sink against what went in, the tap
case compares the tap's window against the tail of the same sink file, and
the events case renders the same scheduled bar twice and compares the C's
own FNV-1a digest.

Written for the build ``clean-build.yml`` makes; it needs no platform
driver.
"""

import os
import struct
import sys
from array import array

import audiocore
import audiomixer
import audiopump
import audioroute
import synthio

RATE = 48000
CHANNELS = 2
BLOCK_FRAMES = 256
BLOCK_BYTES = BLOCK_FRAMES * CHANNELS * 2

TMP = os.getenv("PUMP_PROBE_TMP", "/tmp")

#: ``audiopump.fault()``'s code for a graph that leads back into itself.
FAULT_LOOP = 5

#: Blocks the looped pull is given. The source is four blocks long and the
#: mixer hands out one block at a time, so the loop is traversed well inside
#: this and the pull stops the moment it is.
LOOP_BLOCKS = 40


def status():
    return bytearray(audiopump.STATUS_BYTES)


def words(block):
    return struct.unpack_from("<%dQ" % audiopump.STATUS_WORDS, block, 0)


def say(name, ok, detail):
    print("  %-9s %-4s %s" % (name, "ok" if ok else "FAIL", detail))
    return ok


def tone(frames=BLOCK_FRAMES * 4, step=97):
    data = array("h", bytes(2 * frames * CHANNELS))
    for index in range(frames):
        value = ((index * step) % 4096) - 2048
        for channel in range(CHANNELS):
            data[index * CHANNELS + channel] = value
    return data


def sample(step=97, frames=BLOCK_FRAMES * 4):
    return audiocore.RawSample(tone(frames=frames, step=step),
                               sample_rate=RATE, channel_count=CHANNELS)


def mixer_on(source):
    mixer = audiomixer.Mixer(voice_count=1, sample_rate=RATE,
                             channel_count=CHANNELS, bits_per_sample=16,
                             samples_signed=True, buffer_size=2 * BLOCK_BYTES)
    mixer.voice[0].level = 1.0
    mixer.play(source, voice=0, loop=True)
    return mixer


def pull(tail, blocks, sink=None):
    block = status()
    if sink is None:
        audiopump.pull(tail, blocks, block)
    else:
        audiopump.pull(tail, blocks, block, sink)
    return words(block)


# --- the ring --------------------------------------------------------------


def pattern_bytes(blocks):
    """Bytes that do not repeat inside the run.

    `(index * 7 + 11) & 0xFF` looks fine and has a period of 256, which is
    a whole number of blocks -- so a comparison that lost half its window
    still matched the other half, and `--fault short` read as green. The
    high byte of the index breaks the period.
    """
    data = bytearray()
    for index in range(blocks * BLOCK_BYTES):
        data.append(((index * 7 + 11) ^ (index >> 8)) & 0xFF)
    return data


def digest(data):
    """FNV-1a 64, the same hash the pump keeps in its status block."""
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def ring(fault):
    """What the interpreter pushes in is what the pump pulls out.

    The ring is how a stream with no audiosample behind it -- a decoder, a
    network feed, a file the pump may not touch -- reaches the pump. The
    comparison is against a hash of the bytes that went in, computed here
    in Python, against the hash the C kept of every byte it pulled. No file
    sink: that belongs to the platform driver, and this has to hold on a
    build that has none.
    """
    blocks = 6
    queue = audiopump.Ring(sample_rate=RATE, channel_count=CHANNELS,
                           frames=BLOCK_FRAMES, capacity=blocks + 2)
    pattern = pattern_bytes(blocks)
    wrote = queue.write(memoryview(pattern))
    if fault == "flip":
        pattern[99] ^= 0x01

    got = pull(queue, blocks)
    want = digest(pattern[:wrote])
    same = got[2] == want and wrote == len(pattern) and got[1] == wrote
    return say("ring", same,
               "%d bytes in, %d out, digest %016x / %016x"
               % (wrote, got[1], want, got[2]))


def ring_granularity(fault):
    """A partial frame is refused outright rather than half-written."""
    queue = audiopump.Ring(sample_rate=RATE, channel_count=CHANNELS,
                           frames=BLOCK_FRAMES, capacity=4)
    refused = [n for n in (1, 2, 3) if queue.write(bytes(n)) != 0]
    took = queue.write(bytes(7))          # 7 bytes -> one whole 4-byte frame
    return say("granular", not refused and took == 4,
               "sub-frame writes accepted: %s; a 7-byte write took %d"
               % (refused or "none", took))


# --- the queue -------------------------------------------------------------


def events(fault):
    """A scheduled bar is the same audio every time it is played.

    The queue exists so the groove stops depending on when Python gets round
    to it. The proof is that two runs of the same schedule, rendered on this
    thread with nothing else happening, give the same digest -- and that the
    queue says it applied everything it took.
    """
    digests = []
    counts = []
    for run in range(2):
        synth = synthio.Synthesizer(sample_rate=RATE, channel_count=CHANNELS)
        queue = audiopump.Events(capacity=64)
        audiopump.events(queue)
        envelope = synthio.Envelope(attack_time=0.005, decay_time=0.05,
                                    release_time=0.1, attack_level=0.9,
                                    sustain_level=0.5)
        notes = [synthio.Note(frequency=freq, envelope=envelope)
                 for freq in (220.0, 277.18, 329.63, 440.0)]
        for index, note in enumerate(notes):
            if fault == "drop" and run == 1 and index == 2:
                continue                   # one note never scheduled
            queue.at(index * 4096 + 512, audiopump.PRESS, synth, note)
            queue.at(index * 4096 + 3000, audiopump.RELEASE, synth, note)
        block = status()
        audiopump.pull(synth, 80, block)
        audiopump.events(None)
        digests.append(words(block)[2])
        counts.append(queue.stats())
    same = digests[0] == digests[1]
    applied = counts[0][1]
    return say("events", same and applied > 0 and counts[0][4] == 0,
               "digest %016x / %016x, %d applied, %d late, %d dropped"
               % (digests[0], digests[1], applied, counts[0][2], counts[0][4]))


# --- the tap ---------------------------------------------------------------


def tap(fault):
    """What the tap reads is what actually went out.

    A meter, a scope or a recorder must not be in the audio path to see the
    audio. The tap is a lossy ring the pump writes every block into; this
    fills the graph from a ring whose contents are known byte for byte, so
    the tap's window can be held against the tail of exactly those bytes.
    """
    blocks = 8
    window_blocks = 4
    queue = audiopump.Ring(sample_rate=RATE, channel_count=CHANNELS,
                           frames=BLOCK_FRAMES, capacity=blocks + 2)
    pattern = pattern_bytes(blocks)
    wrote = queue.write(memoryview(pattern))

    probe = audiopump.Tap(frames=window_blocks * BLOCK_FRAMES)
    audiopump.tap(probe)
    pull(queue, blocks)
    audiopump.tap(None)

    window = bytearray(window_blocks * BLOCK_BYTES)
    got = probe.readinto(window)
    if fault == "short":
        got = got // 2
    tail = bytes(pattern[wrote - got:]) if got else b""
    same = got > 0 and bytes(window[:got]) == tail
    return say("tap", same,
               "%d bytes of tap against the last %d of %d pulled: %s"
               % (got, len(tail), wrote, "same" if same else "DIFFERENT"))


# --- the port --------------------------------------------------------------


def port(fault):
    """A port pulled while already inside itself must fault, not recurse.

    The mistake reads like an improvement -- ``self._output =
    Wrap(self._output)`` on something that is already a port -- and what it
    builds is a graph that leads back through itself. Pulling it recurses:
    on a desktop a stack overflow and a core dump, on a board the pump
    thread never returning and the audio stopping dead with nothing printed.
    Every node in the ring is native, so no Python guard can see it.

    Both doors into the guard are checked, told apart by when they fire.
    A ring of ports faults on block 0 -- the pull walks into it. A looping
    mixer voice faults blocks later, because it rewinds its source before it
    asks for it, and that rewind is ``reset_buffer``: a port that guards
    only ``get_buffer`` still recurses there, and the C says so at the top
    of ``audioroute_port_reset_buffer``.
    """
    blocks = 120
    bare = mixer_on(sample())
    plain = audioroute.Port(mixer_on(sample()))
    bare_words = pull(bare, blocks)
    plain_words = pull(plain, blocks)
    transparent = bare_words[2] == plain_words[2]

    # Two loops, told apart by WHEN they fault rather than by the code they
    # set, which is the same for either. Reaching the end of this at all is
    # half the check: unguarded, either one is a stack overflow here and a
    # pump thread that never returns on a board.
    #
    # The source also has to RUN OUT inside the pull window, or the loop is
    # never traversed and the guard never has anything to catch -- a
    # RawSample hands back its whole buffer in one call, and a longer one
    # let the first draft of this read as green.
    doors = {}

    # A ring of ports and nothing else: the first pull walks straight into
    # it, before a block of audio exists.
    audiopump.lock_reset()
    first = audioroute.Port(sample())
    second = audioroute.Port(sample())
    first.play(second)
    second.play(first)
    got = pull(first, LOOP_BLOCKS)
    doors["pull"] = (got[24], got[0], got[5])

    # The rewind. `mixer.play()` primes the voice while the port's source is
    # still the RawSample, so four blocks of audio come out before the voice
    # needs more -- and a LOOPING voice rewinds its source before it asks
    # for it, which is reset_buffer, not get_buffer. That is the one a guard
    # on the pull alone leaves in place.
    audiopump.lock_reset()
    looped = audioroute.Port(sample())
    mixer = audiomixer.Mixer(voice_count=1, sample_rate=RATE,
                             channel_count=CHANNELS, bits_per_sample=16,
                             samples_signed=True,
                             buffer_size=2 * BLOCK_BYTES)
    mixer.voice[0].level = 1.0
    mixer.play(looped, voice=0, loop=True)
    looped.play(mixer)                     # the loop closes here
    got = pull(looped, LOOP_BLOCKS)
    doors["rewind"] = (got[24], got[0], got[5])

    faulted = all(code == FAULT_LOOP and err != 0
                  for code, _at, err in doors.values())
    # The rewind must fire AFTER the primed blocks and the pull on the
    # first one. Same-block faults for both would mean one construction was
    # standing in for the other and the other was never tried.
    distinct = doors["pull"][1] == 0 < doors["rewind"][1]

    # The flag is not a latch: take the loop out and the port works again.
    audiopump.lock_reset()
    if fault != "latch":
        looped.play(mixer_on(sample()))
    again = pull(looped, blocks)
    recovered = (again[0] == blocks and again[5] == 0
                 and again[2] == plain_words[2])

    ok = transparent and faulted and distinct and recovered
    return say("port", ok,
               "transparent=%s fault=%d at block %d (the pull) / "
               "fault=%d at block %d (a rewind) recovered=%s"
               % (transparent, doors["pull"][0], doors["pull"][1],
                  doors["rewind"][0], doors["rewind"][1], recovered))


def refused(fault):
    """audiodsp#127, and audiocomponents#96. A SCHEDULED press that finds no
    free channel is counted, not lost.

    The live path was never the whole story: `audiopump_apply_press` throws
    away what `synthio_span_change_note` returns, so a press applied from the
    queue with every channel held left no trace at all. An 8-step gate over
    8-note chords lost 488 presses on juno106 and 1976 on solina before
    anything could say so.

    One counter answers for both paths, because both come through
    `change_note`. This schedules more presses than there are channels,
    without releasing any, and reads `synth.refused`.

    `--fault deaf` asserts the counter stays at zero, which must fail.
    """
    synth = synthio.Synthesizer(sample_rate=RATE, channel_count=CHANNELS)
    ceiling = synth.max_polyphony
    over = 9
    queue = audiopump.Events(capacity=ceiling + over + 4)
    audiopump.events(queue)
    notes = [synthio.Note(frequency=110.0 + step)
             for step in range(ceiling + over)]
    for index, note in enumerate(notes):
        # All at frame 0 and never released, so the last `over` of them
        # arrive at a synthesizer with every channel held.
        queue.at(0, audiopump.PRESS, synth, note)
    audiopump.pull(synth, 4, status())
    stats = queue.stats()
    audiopump.events(None)

    counted = 0 if fault == "deaf" else synth.refused
    # The queue applied every event: a refused press is an event that landed
    # perfectly and had nowhere to put its note, which is a different number
    # from the queue's own `refused`.
    applied = stats[1]
    return say("refused", counted == over and applied == len(notes),
               "%d scheduled presses into %d channels: synth.refused=%d "
               "(want %d), queue applied %d of %d"
               % (len(notes), ceiling, counted, over, applied, len(notes)))


CASES = (("ring", ring), ("granular", ring_granularity),
         ("events", events), ("tap", tap), ("port", port),
         ("refused", refused))


def main():
    args = sys.argv[1:]
    fault = None
    if "--fault" in args:
        index = args.index("--fault")
        fault = args[index + 1]
        del args[index:index + 2]
    wanted = args or [name for name, _run in CASES]

    print("audiodsp: the ring, the queue, the tap and the port   fault: %s"
          % fault)
    ok = True
    for name, run in CASES:
        if name not in wanted:
            continue
        ok = run(fault) and ok
    print("VERDICT:", "kept" if ok else "BROKEN")
    return 0 if ok else 1


sys.exit(main())
