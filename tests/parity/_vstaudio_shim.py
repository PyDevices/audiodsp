# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""A stand-in for the `vstaudio` host module, so the original instrument
scripts can be imported and played outside a VST3 sidecar.

Only what those scripts touch is here: the sample rate they build their tables
from, the two registration calls they end with, the transport reading the
tempo-synced ones poll, and the event constants. It exists so the parity probes
can capture what the scripts sounded like before they were ported.
"""

EVENT_NOTE_ON = 1
EVENT_NOTE_OFF = 2
EVENT_POLY_PRESSURE = 3
EVENT_PITCH_BEND = 4
EVENT_CONTROL_CHANGE = 5
EVENT_PARAMETER = 6
EVENT_CHANNEL_PRESSURE = 7
EVENT_TRANSPORT = 8
EVENT_PROGRAM_CHANGE = 9

_sample_rate = 48000
_handler = None
_output = None
_transport = (False, 0.0, 120.0, 4, 4)


def _reset(rate):
    global _sample_rate, _handler, _output, _transport
    _sample_rate = int(rate)
    _handler = None
    _output = None
    _transport = (False, 0.0, 120.0, 4, 4)


def sample_rate():
    return _sample_rate


def on_event(handler):
    global _handler
    _handler = handler


def output(sample):
    global _output
    _output = sample


def clear_output():
    global _output, _handler
    _output = None
    _handler = None


def transport():
    return _transport


def error(_message):
    pass


def input():
    raise RuntimeError("no host input in the parity harness")


def input_stats():
    return (0, 0, 0, False)
