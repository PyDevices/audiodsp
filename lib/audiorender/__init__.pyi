# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Render a whole composition offline, through audiodsp's own DSP.

Tracks, a tempo map, notes and automation go in; a mixed stereo master and the
level report it is judged by come out. It is offline and desktop-only: it
needs numpy and holds the whole song in memory.
"""

from .events import MACRO as MACRO
from .events import MACRO_COUNT as MACRO_COUNT
from .events import NOTE_OFF as NOTE_OFF
from .events import NOTE_ON as NOTE_ON
from .events import ORDER as ORDER
from .events import PROGRAM as PROGRAM
from .events import build_events as build_events
from .events import deliver as deliver
from .metrics import busiest_beat as busiest_beat
from .metrics import db as db
from .metrics import report as report
from .metrics import section_profile as section_profile
from .metrics import section_table as section_table
from .render import BLOCK as BLOCK
from .render import Master as Master
from .render import apply_mix as apply_mix
from .render import render as render
from .render import render_track as render_track
from .tempo import TempoMap as TempoMap
from .voices import Clock as Clock
from .voices import PcmSource as PcmSource
from .voices import Puller as Puller
from .voices import Voice as Voice
from .wav import write_wav as write_wav

__all__ = [
    "BLOCK", "Clock", "MACRO", "MACRO_COUNT", "Master", "NOTE_OFF", "NOTE_ON", "ORDER", "PROGRAM",
    "PcmSource", "Puller", "TempoMap", "Voice", "apply_mix", "build_events", "busiest_beat", "db",
    "deliver", "render", "render_track", "report", "section_profile", "section_table", "write_wav",
]
