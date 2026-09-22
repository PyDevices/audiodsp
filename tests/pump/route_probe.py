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
import audiomodal
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


def excitation(strike_at, frames=BLOCK_FRAMES * 16):
    """Two clicks: one at the head and one at `strike_at`.

    A modal bank is a filter, not a generator -- it rings what is fed INTO
    it. Feeding silence and setting gains produces nothing at all, which is
    what the first version of this case measured. So the first click arrives
    while the gains are still zero (the bank must stay silent) and the second
    arrives with the strike (it must ring).
    """
    values = array("h", bytes(frames * CHANNELS * 2))
    for at in (0, strike_at):
        for channel in range(CHANNELS):
            values[at * CHANNELS + channel] = 20000
    return audiocore.RawSample(values, sample_rate=RATE,
                               channel_count=CHANNELS)


def _tap_peak(probe, window):
    """Peak magnitude in the tap's window, -1 if it had nothing new."""
    got = probe.readinto(window)
    if got == 0:
        return -1
    peak = 0
    for at in range(0, got, 2):
        value = window[at] | (window[at + 1] << 8)
        if value >= 32768:
            value -= 65536
        peak = max(peak, abs(value))
    return peak


def strike(fault):
    """audiocomponents#94. A strike on a modal bank can be held to a frame.

    A strike there is a retune of a node that is already running -- writing
    the mode gains injects the energy the moment the excitation arrives -- so
    it is a C state change and not a press, and no queue could hold it back
    until it had a frame of its own. `acoustickit` is `schedulable=False` for
    that reason and no other.

    Three claims, in the order they matter:

      same    two runs of one schedule render the same bytes, which is what
              "scheduled" has to mean at all;
      late    the bank is SILENT through a click that arrives before the
              strike's frame, and rings on the one that arrives with it;
      choke   a scheduled CHOKE empties a bank that is ringing out, which is
              what a closing hi-hat does to the open one.

    `--fault early` moves the strike to frame 0, so the first click rings
    too and the "silent before" row fails. Without it the determinism row
    would pass on a bank that struck immediately and prove nothing about
    when.
    """
    modes = 4
    table = array("f")
    for partial in range(modes):
        table.extend((220.0 * (partial + 1), 0.45, 0.35))

    at = 0 if fault == "early" else 4 * BLOCK_FRAMES

    def fresh():
        node = audiomodal.Bank(modes=modes, sample_rate=RATE,
                               channel_count=CHANNELS, mix=1.0, gain=1.0)
        node.set_modes([(220.0 * (p + 1), 0.45, 0.0) for p in range(modes)])
        node.play(excitation(at))
        return node

    digests = []
    for _run in range(2):
        bank = fresh()
        queue = audiopump.Events(capacity=8)
        audiopump.events(queue)
        queue.at(at, audiopump.STRIKE, bank, table)
        block = status()
        audiopump.pull(bank, 12, block)
        audiopump.events(None)
        digests.append(words(block)[2])
    ok = say("strike", digests[0] == digests[1],
             "two runs of one schedule: %016x / %016x"
             % (digests[0], digests[1]))

    window = bytearray(2 * BLOCK_BYTES)
    bank = fresh()
    queue = audiopump.Events(capacity=8)
    audiopump.events(queue)
    queue.at(at, audiopump.STRIKE, bank, table)
    probe = audiopump.Tap(frames=2 * BLOCK_FRAMES)
    audiopump.tap(probe)
    audiopump.pull(bank, 3, status())          # the first click has passed
    before = _tap_peak(probe, window)
    audiopump.pull(bank, 5, status())          # now past the strike
    after = _tap_peak(probe, window)
    audiopump.tap(None)
    audiopump.events(None)
    ok = say("strike late", before == 0 and after > 0,
             "peak through the click before its frame=%d, after the "
             "strike=%d" % (before, after)) and ok

    # The choke. Strike at once, let it ring out with nothing more feeding
    # it, then empty it.
    bank = audiomodal.Bank(modes=modes, sample_rate=RATE,
                           channel_count=CHANNELS, mix=1.0, gain=1.0)
    bank.set_modes([(220.0 * (p + 1), 2.0, 0.35) for p in range(modes)])
    bank.play(excitation(0))
    probe = audiopump.Tap(frames=2 * BLOCK_FRAMES)
    audiopump.tap(probe)
    queue = audiopump.Events(capacity=8)
    audiopump.events(queue)
    audiopump.pull(bank, 4, status())
    ringing = _tap_peak(probe, window)
    queue.at(0, audiopump.CHOKE, bank)
    audiopump.pull(bank, 4, status())
    silenced = _tap_peak(probe, window)
    audiopump.tap(None)
    audiopump.events(None)
    ok = say("strike choke", ringing > 0 and silenced == 0,
             "ringing peak=%d, after a scheduled CHOKE=%d"
             % (ringing, silenced)) and ok
    return ok


CASES = (("ring", ring), ("granular", ring_granularity),
         ("events", events), ("tap", tap), ("port", port),
         ("strike", strike))


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
