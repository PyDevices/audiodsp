# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Turn a track's notes and automation into a time-ordered event list.

Events are ``(sample_position, kind, data, value)`` where `kind` is one of
the constants below and `value` is normalized 0.0-1.0 for macros, or a
velocity for a note-on.
"""

NOTE_OFF = 0
NOTE_ON = 1
MACRO = 6
PROGRAM = 9

#: Order at a shared sample position. Getting this wrong is silent: with
#: the program change ahead of the initial macro block, the patch is
#: applied and then immediately overwritten by the fallback, and the
#: render disagrees with what a host would play by several dB.
#:
#: Macro values first, then a program change (which replaces all of them),
#: then note-offs, then note-ons.
ORDER = {MACRO: 0, PROGRAM: 1, NOTE_OFF: 2, NOTE_ON: 3}

#: How many macros a track may address. Sixteen is what the format
#: carries; a track that sets more is a mistake worth failing on.
MACRO_COUNT = 16


def build_events(track, composition, tempo, patch=None, block=256,
                 macro_count=MACRO_COUNT):
    """Every event for one track, sorted by position then by kind.

    `patch` is the instrument's patch 0 as ``{index: normalized value}``.
    A macro the composition does not mention resolves there rather than to
    the middle of its range, which is not "off" and not what the
    instrument's author intended - so it has to agree with whatever a host
    would load, or the render stops being a check on the host.
    """
    rate = tempo.sample_rate
    patch = patch or {}
    events = []

    for index in range(macro_count):
        if index in track["macros"] or index in track["macro_env"]:
            value = composition.macro_value(track, index, 0.0)
        else:
            value = patch.get(index, 0.5)
        events.append((0, MACRO, index, value))

    for start, duration, pitch, velocity in track["notes"]:
        first = int(composition.beats_to_seconds(start) * rate)
        last = int(composition.beats_to_seconds(start + duration) * rate)
        events.append((first, NOTE_ON, pitch, velocity))
        # A zero-length note still has to end after it starts, or the
        # note-off sorts ahead of its own note-on.
        events.append((max(first + 1, last), NOTE_OFF, pitch, 0.0))

    # Macro automation, sampled per block for as long as it is moving.
    for index, envelope in track["macro_env"].items():
        if not envelope:
            continue
        sample = int(composition.beats_to_seconds(envelope[0][0]) * rate)
        end = int(composition.beats_to_seconds(envelope[-1][0]) * rate)
        previous = None
        while sample <= end:
            value = composition.macro_value(
                track, index, tempo.beat_at_sample(sample))
            if previous is None or abs(value - previous) > 0.002:
                events.append((sample, MACRO, index, value))
                previous = value
            sample += block

    for start, program in track.get("programs", ()):
        events.append((int(composition.beats_to_seconds(start) * rate),
                       PROGRAM, program, 0.0))

    events.sort(key=lambda event: (event[0], ORDER[event[1]]))
    return events


def deliver(instrument, event, sample_position):
    """Play one event on an `audioinstruments` instrument.

    Values are normalized here and MIDI on the instrument's side, so this
    is where the scale changes - a multiply, not a quantization, so
    automation finer than 7 bits keeps its resolution.
    """
    _, kind, data, value = event
    if kind == NOTE_ON:
        instrument.note_on(data, _midi_byte(value),
                           sample_position=sample_position)
    elif kind == NOTE_OFF:
        instrument.note_off(data, sample_position=sample_position)
    elif kind == MACRO:
        instrument.set_macro(data, value * 127.0,
                             sample_position=sample_position)
    elif kind == PROGRAM:
        instrument.program_change(data, sample_position=sample_position)


def _midi_byte(value):
    """Convert the renderer's normalized scalar to a MIDI data byte."""
    value = max(0.0, min(1.0, float(value)))
    return int(value * 127.0 + 0.5)
