"""CircuitPython 11.0.0-alpha.1's synthio, audiomixer and audiocore fixes.

    cp11_fixes_probe.py synthio

Each case is one fix upstream made after 10.3.0 that this port took when it
resynced with 11.0.0-alpha.1 (audiodsp#220). Every interpreter prints the same
lines, and each case prints something different on a build without its fix:

- ring modulation of two troughs is +32767, not a full-scale sign flip
  (904e7a7a55);
- a note past Nyquist for its waveform LOOP is not played, where 10.3.0
  compared the rate with the whole waveform and let an aliased tone through;
- a ring is held to Nyquist for its own loop, not the main waveform's, both
  ways round;
- a MIDI track cut off inside an event reports the error where the data
  ends, and reads nothing past it;
- tempo and sample_rate are divisors and refuse 0 in the constructors, and
  the sample_rate setter every sample shares refuses it too (b34aa34c19);
- synthio.from_file keeps the track it read, and reads it from the top of a
  file that was already part-read;
- Mixer.play(voice=256) is refused, not played on voice 0.

An argument error prints only the exception's type: the messages are each
target's own.
"""

import gc
import os
import sys
from array import array

import audiocore
import audiomixer

SYNTHIO = __import__(sys.argv[1] if len(sys.argv) > 1 else "synthio")

RATE = 8000
FLAT = SYNTHIO.Envelope(attack_time=0, decay_time=0, release_time=0,
                        attack_level=1.0, sustain_level=1.0)


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def ramp(length, low=-30000, high=30000):
    return array("h", (low + (high - low) * index // (length - 1)
                       for index in range(length)))


def render(tag, synth, count=2):
    for index in range(count):
        result, view = audiocore.get_buffer(synth)
        data = bytes(view)
        samples = array("h", data)
        print(tag, index, result, len(data), checksum(data),
              min(samples) if samples else 0, max(samples) if samples else 0)


def play(tag, note, count=2):
    synth = SYNTHIO.Synthesizer(sample_rate=RATE, envelope=FLAT)
    synth.press(note)
    render(tag, synth, count)


def refused(tag, action):
    try:
        action()
    except Exception as error:  # noqa: BLE001 - the type is what is printed
        print(tag, "refused", type(error).__name__)
    else:
        print(tag, "accepted")


# Two troughs: -32768 * -32768 / 32768 is +32768, which an int16_t holds as
# -32768.
trough = array("h", [-32768] * 64)
play("ring_trough", SYNTHIO.Note(frequency=125, waveform=trough,
                                 ring_frequency=125, ring_waveform=trough))

# A loop of 64 samples at 8 kHz has its Nyquist at 4 kHz. 6 kHz is 48 samples
# a step: past half the loop, inside half the 256-sample table.
play("loop_nyquist", SYNTHIO.Note(frequency=6000, waveform=ramp(256),
                                  waveform_loop_start=192,
                                  waveform_loop_end=256))

# A short main table and a long ring: 1 kHz on 512 samples is 64 a step,
# inside the ring's own Nyquist (256) and past the main table's (8).
play("ring_long", SYNTHIO.Note(frequency=100, waveform=ramp(16),
                               ring_frequency=1000, ring_waveform=ramp(512)))

# And the other way: a 64-sample ring loop at 6 kHz is 48 a step, past the
# loop's Nyquist (32), inside the main table's (512).
play("ring_loop", SYNTHIO.Note(frequency=100, waveform=ramp(1024),
                               ring_frequency=6000, ring_waveform=ramp(512),
                               ring_waveform_loop_start=448,
                               ring_waveform_loop_end=512))

# One status byte and nothing else: the error is at 1, where data ends.
bad = SYNTHIO.MidiTrack(b"\x80", 640, sample_rate=RATE)
result, view = audiocore.get_buffer(bad)
print("midi_status_only", result, len(bytes(view)), bad.error_location)

# A note-on whose velocity is missing.
cut = SYNTHIO.MidiTrack(b"\x00\x90\x3c", 640, sample_rate=RATE, envelope=FLAT)
result, view = audiocore.get_buffer(cut)
data = bytes(view)
print("midi_cut_note", result, len(data), checksum(data), cut.error_location)

refused("miditrack_tempo_0",
        lambda: SYNTHIO.MidiTrack(b"\x00\xff", 0, sample_rate=RATE))
refused("synthesizer_rate_0", lambda: SYNTHIO.Synthesizer(sample_rate=0))

raw = audiocore.RawSample(array("h", [0] * 64), sample_rate=RATE)
refused("rawsample_set_rate_0", lambda: setattr(raw, "sample_rate", 0))
synth = SYNTHIO.Synthesizer(sample_rate=RATE)
refused("synthesizer_set_rate_0", lambda: setattr(synth, "sample_rate", 0))
print("rates_kept", raw.sample_rate, synth.sample_rate)

mixer = audiomixer.Mixer(voice_count=2, sample_rate=RATE, channel_count=1,
                         bits_per_sample=16, samples_signed=True)
refused("mixer_play_voice_256",
        lambda: mixer.play(raw, voice=256, loop=True))
print("mixer_playing", mixer.voice[0].playing, mixer.voice[1].playing)
refused("mixer_stop_voice_256", lambda: mixer.stop_voice(voice=256))

# A one-track file: two notes, each held then released.
track = bytes((
    0, 0x90, 60, 100,
    0x40, 0x80, 60, 0,
    0, 0x90, 67, 100,
    0x40, 0x80, 67, 0,
    0, 0xFF, 0x2F, 0,
))
header = b"MThd\x00\x00\x00\x06\x00\x00\x00\x01\x00\x60"
chunk = b"MTrk" + bytes((0, 0, 0, len(track)))
NAME = "cp11_fixes_probe.mid"
with open(NAME, "wb") as handle:
    handle.write(header + chunk + track)
try:
    with open(NAME, "rb") as handle:
        midi = SYNTHIO.from_file(handle, sample_rate=RATE, envelope=FLAT)
    # Reuse whatever the loader may have handed back to the allocator.
    litter = [bytearray(b"\xff" * len(track)) for _ in range(64)]
    gc.collect()
    total, blocks, value = 0, 0, 2166136261
    while blocks < 64:
        result, view = audiocore.get_buffer(midi)
        data = bytes(view)
        for byte in data:
            value = ((value ^ byte) * 16777619) & 0xffffffff
        total += len(data)
        blocks += 1
        if result != 1:
            break
    print("from_file", result, blocks, total, value, midi.error_location)
    del litter

    # Part-read first: the loader starts from the top.
    with open(NAME, "rb") as handle:
        handle.read(5)
        again = SYNTHIO.from_file(handle, sample_rate=RATE, envelope=FLAT)
    result, view = audiocore.get_buffer(again)
    data = bytes(view)
    print("from_file_part_read", result, len(data), checksum(data))
finally:
    os.remove(NAME)
