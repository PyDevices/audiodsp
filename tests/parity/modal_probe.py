"""Deterministic audiomodal PCM, and the three invariants the module exists for.

    modal_probe.py audiomodal

Like filter_f32_probe.py and feedback_delay_probe.py, this has no oracle:
`audiomodal` is audiodsp's own module, with no ancestor in CircuitPython or in
micropython-vst3's engine. What the gate pins is that every interpreter
renders it identically, and that nothing moves it by accident later.

Three of the sections are invariants rather than PCM, because they are the
whole ask:

- **tail** prints the first output block that is all zero after the source
  goes silent, and it is what makes the skip in the process loop honest. A
  mode that merely decayed toward zero would keep the recursion running for
  ever and the "this drum has stopped" test would be a threshold rather than
  a fact. A negative number here means the tail never arrived, and that is a
  failure.
- **rings** prints whether the bank is still sounding several blocks after
  the source ran out. It is the opposite claim to the delay's and the tank's,
  both of which stop when their source does: a struck object goes on ringing
  after the stick has left, and a bank that agreed with `audioecho` here
  would have failed.
- **highq** prints the peak of a mode whose decay puts it far past anything
  `audiobiquad` will accept -- Q of about four thousand against its cap of
  sixty. The number is not interesting in itself; that it is finite, equal on
  three interpreters, and not zero is the claim.

Everything a probe prints has to be integer-exact on three interpreters, so
the excitation is an integer impulse and the mode table is written in exact
binary fractions -- never `math.sin()`, which is three functions that nearly
agree. Nothing here drives a parameter with a `synthio` block for the same
reason: the block layer is C on two targets and Python on the third, and a
gate that hashed it would be measuring that seam rather than this module's.
"""

import sys
from array import array

import audiocore

MODULE = sys.argv[1] if len(sys.argv) > 1 else "audiomodal"
# A built-in module under MicroPython, which does not record those in
# sys.modules - take what __import__ hands back.
modal = __import__(MODULE)

SAMPLE_RATE = 8000

#: (frequency, decay, gain) per mode. Frequencies are whole hertz and decays
#: and gains are exact binary fractions, so every interpreter parses the same
#: float and the coefficients are computed from identical inputs.
KICK = (
    (55.0, 0.5, 1.0),
    (87.0, 0.25, 0.5),
    (117.0, 0.125, 0.25),
    (125.0, 0.0625, 0.125),
)

#: One mode a filter cannot be asked for: 2 kHz ringing for four seconds is
#: Q = pi * f * T60 / ln(1000) = 3638, sixty times audiobiquad's cap of 60.
HIGH_Q = (2000.0, 4.0, 1.0)


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def impulse(frames=1600, level=12000, channels=2):
    """One sample and then nothing -- the stick, in its simplest form."""
    values = array("h", bytes(frames * channels * 2))
    for channel in range(channels):
        values[channel] = level
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def silence(frames=1600, channels=2):
    values = array("h", bytes(frames * channels * 2))
    return audiocore.RawSample(values, sample_rate=SAMPLE_RATE,
                               channel_count=channels)


def build(table, channels=2):
    bank = modal.Bank(modes=len(table), sample_rate=SAMPLE_RATE,
                      channel_count=channels)
    bank.set_modes(table)
    return bank


def emit(tag, node, blocks):
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(node)[1])
        print("modal", tag, index, len(data), sum(data), checksum(data))


def tail(tag, table, channels=2, blocks=600):
    """First all-zero block after the source goes quiet, or -1 for never."""
    bank = build(table, channels)
    bank.play(impulse(channels=channels))
    audiocore.get_buffer(bank)
    bank.play(silence(channels=channels))
    quiet = -1
    for index in range(blocks):
        data = bytes(audiocore.get_buffer(bank)[1])
        if data != bytes(len(data)):
            quiet = -1
        elif quiet < 0:
            quiet = index
    print("tail", tag, quiet)


def rings(tag, table, channels=2, blocks=8):
    """Non-zero output after the source is gone. A delay prints 0 here."""
    bank = build(table, channels)
    bank.play(impulse(channels=channels))
    audiocore.get_buffer(bank)
    bank.stop() if hasattr(bank, "stop") else None
    loudest = 0
    for _index in range(blocks):
        data = bytes(audiocore.get_buffer(bank)[1])
        for position in range(0, len(data), 2):
            word = data[position] | (data[position + 1] << 8)
            if word >= 32768:
                word -= 65536
            if word < 0:
                word = -word
            if word > loudest:
                loudest = word
    print("rings", tag, 1 if loudest > 0 else 0)


def peak(tag, table, channels=1, blocks=20):
    bank = build(table, channels)
    bank.play(impulse(channels=channels))
    loudest = 0
    for _index in range(blocks):
        data = bytes(audiocore.get_buffer(bank)[1])
        for position in range(0, len(data), 2):
            word = data[position] | (data[position + 1] << 8)
            if word >= 32768:
                word -= 65536
            if word < 0:
                word = -word
            if word > loudest:
                loudest = word
    print("peak", tag, loudest)


def main():
    for channels in (1, 2):
        bank = build(KICK, channels)
        bank.play(impulse(channels=channels))
        emit("kick%d" % channels, bank, 6)

    # A bank with every mode silent is a wire: the claim is that an
    # unconfigured Bank does not invent a chord out of whatever was in memory.
    quiet_bank = modal.Bank(modes=4, sample_rate=SAMPLE_RATE, channel_count=2)
    quiet_bank.play(impulse())
    emit("unset", quiet_bank, 2)

    # mix=0 is the untouched signal, exactly. If this ever stops matching the
    # source it means the dry path is being routed through the arithmetic.
    dry = build(KICK, 2)
    dry.set(mix=0.0)
    dry.play(impulse())
    emit("dry", dry, 2)

    tail("kick", KICK)
    rings("kick", KICK)
    peak("highq", (HIGH_Q,))


main()
