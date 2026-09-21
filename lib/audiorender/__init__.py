"""Render a whole composition offline, through audiodsp's own DSP.

This is the tier above :mod:`audioinstruments` and :mod:`audioeffects`: it
knows what a *piece* is - tracks, a tempo map, notes, automation, sections
- and turns one into a mixed stereo master plus the level report that
master is judged by. It is deliberately offline and CPython-shaped (numpy
throughout, a whole song held in memory); the packages below it are the
ones that run on a device.

A **composition** is any module or object carrying:

===========================  =================================================
``TITLE``                    what to call the piece
``SAMPLE_RATE``              frames per second
``MASTER_GAIN_DB``           applied to the sum of every track
``TEMPO_MAP``                ``[(beat, bpm[, ts_num, ts_den]), ...]``
``TOTAL_BEATS``              where the piece ends
``SONG_SECONDS``             the same point in seconds
``RENDER_SECONDS``           how long to render, tail included
``SECTIONS``                 ``[(name, first beat, last beat), ...]``
``TRACKS``                   see below
``ACTIVE_LIMIT``             optional cap on simultaneous tracks
``beats_to_seconds(beat)``   the tempo map, applied
``macro_value(track,
  index, beat)``             a macro at a moment, normalized 0.0-1.0
``track_gain(track, beat)``  the fader at a moment, as amplitude
``active_track_count(beat)`` how many tracks sound at that beat
===========================  =================================================

A **track** is a dict with ``name``, ``script``, ``pan`` (-1.0 to 1.0),
``notes`` (``[(start beat, duration, pitch, velocity), ...]``), ``macros``,
``macro_env``, optional ``programs`` and ``effects``.

Loading a track's sound is the caller's job, not this package's - a
plug-in host has its own script loader and its own reasons to keep it. Pass
:func:`render` a ``voice_for`` that returns a :class:`~.voices.Voice`
around an :mod:`audioinstruments` instrument, or anything else that speaks
the small protocol in :mod:`.voices`.

    from audiorender import Clock, Voice, render, report, write_wav

    master = render(composition,
                    lambda track, clock: Voice(build(track, clock)),
                    out=print)
    report(master)
    write_wav("out.wav", master.data, master.sample_rate)
"""

from .events import (MACRO, MACRO_COUNT, NOTE_OFF, NOTE_ON, ORDER, PROGRAM,
                     build_events, deliver)
from .metrics import busiest_beat, db, report, section_profile, section_table
from .render import BLOCK, Master, apply_mix, render, render_track
from .tempo import TempoMap
from .voices import Clock, PcmSource, Puller, Voice
from .wav import write_wav

__all__ = (
    "BLOCK", "Clock", "MACRO", "MACRO_COUNT", "Master", "NOTE_OFF", "NOTE_ON",
    "ORDER", "PROGRAM", "PcmSource", "Puller", "TempoMap", "Voice",
    "apply_mix", "build_events", "busiest_beat", "db", "deliver", "render",
    "render_track", "report", "section_profile", "section_table", "write_wav",
)
