# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""MIDI event parser and renderer parity probe."""

from array import array

import audiocore
import synthio


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


wave = array("h", (-32768, -12000, 10000, 32767))
events = bytes((
    0, 0x90, 60, 100,
    0x20, 0x90, 64, 80,
    0x30, 0x80, 60, 0,
    0, 0xB0, 7, 90,
    0x20, 0x80, 64, 0,
    0, 0xFF,
))
track = synthio.MidiTrack(
    events, 640, sample_rate=8000, waveform=wave,
    envelope=synthio.Envelope(
        attack_time=.01, decay_time=.02, sustain_level=.7,
        release_time=.03,
    ),
)
for index in range(12):
    result, view = audiocore.get_buffer(track)
    data = bytes(view)
    print("midi", index, result, len(data), sum(data), checksum(data),
          track.error_location)
    if index == 2:
        track.tempo = 800
    if result == 0:
        break

audiocore.reset_buffer(track)
result, view = audiocore.get_buffer(track)
data = bytes(view)
print("midi_reset", result, len(data), sum(data), checksum(data),
      track.error_location, track.tempo)

bad = synthio.MidiTrack(b"\x80", 640, sample_rate=8000)
result, view = audiocore.get_buffer(bad)
print("midi_bad", result, len(view), bad.error_location)
