# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Six-operator FM synthesis that plays the classic six-operator voice format.

`Synth` is an audio source with a pool of voices: press and release notes on
it and play it like any other source. `Patch` is one voice, with every
parameter as an attribute, and reads and writes the 155-byte single voice and
the 4104-byte 32-voice bank the format's instruments and editors exchange.

    bank = audiofm.Patch.bank(open("bank.syx", "rb").read())
    synth = audiofm.Synth(voices=16, sample_rate=48000, patch=bank[0])
    synth.press(60, 100)

The engine runs in fixed point and renders identically here and on
MicroPython; this file is the CPython binding over the same C kernel.
"""

from audiocore import GET_BUFFER_MORE_DATA, _AudioSample
import _audiodsp


#: The audiodsp this was built from, the same pair the native builds put
#: on this module.
__version__ = _audiodsp.__version__
__revision__ = _audiodsp.__revision__

FRAMES = _audiodsp.FM_FRAMES
MAX_VOICES = _audiodsp.FM_MAX_VOICES

_PATCH_BYTES = 156
_VOICE_BYTES = 155
_PACKED_BYTES = 128
_BANK_SYSEX = 4104
_BANK_DATA = 4096
_VOICE_SYSEX = 163
_MAX = _audiodsp.fm_patch_max()

# Operator fields: name -> (offset inside the operator's 21 bytes, kind).
# A kind of 4 is a tuple of four bytes, "b" a flag, anything else one byte.
_OP_FIELDS = {
    "rates": (0, 4),
    "levels": (4, 4),
    "breakpoint": (8, 1),
    "left_depth": (9, 1),
    "right_depth": (10, 1),
    "left_curve": (11, 1),
    "right_curve": (12, 1),
    "rate_scaling": (13, 1),
    "amp_mod": (14, 1),
    "velocity": (15, 1),
    "level": (16, 1),
    "fixed": (17, "b"),
    "coarse": (18, 1),
    "fine": (19, 1),
    "detune": (20, 1),
}

# Voice fields: name -> (offset, kind).
_PATCH_FIELDS = {
    "pitch_rates": (126, 4),
    "pitch_levels": (130, 4),
    "feedback": (135, 1),
    "key_sync": (136, "b"),
    "lfo_speed": (137, 1),
    "lfo_delay": (138, 1),
    "lfo_pitch_depth": (139, 1),
    "lfo_amp_depth": (140, 1),
    "lfo_key_sync": (141, "b"),
    "lfo_wave": (142, 1),
    "lfo_pitch_sens": (143, 1),
    "transpose": (144, 1),
}


def _get(data, offset, kind):
    if kind == 4:
        return tuple(data[offset:offset + 4])
    if kind == "b":
        return bool(data[offset])
    return data[offset]


def _set(data, offset, kind, name, value):
    if kind == 4:
        values = tuple(value)
        if len(values) != 4:
            raise ValueError("%s takes four values" % name)
    elif kind == "b":
        values = (1 if value else 0,)
    else:
        values = (value,)
    for index, item in enumerate(values):
        item = int(item)
        top = _MAX[offset + index]
        if item < 0 or item > top:
            raise ValueError("%s must be 0 to %d" % (name, top))
        data[offset + index] = item


class Operator:
    """One of a `Patch`'s six operators, read and written in place."""

    def __init__(self):
        raise TypeError("operators come from Patch.operators")

    @classmethod
    def _of(cls, patch, number):
        op = cls.__new__(cls)
        op._data = patch._data
        op._number = number
        op._base = (6 - number) * 21
        return op

    @property
    def number(self):
        """Which operator this is, 1 to 6."""
        return self._number

    @property
    def enabled(self):
        """False mutes the operator without changing its settings."""
        return bool(self._data[155] & (1 << (6 - self._number)))

    @enabled.setter
    def enabled(self, value):
        bit = 1 << (6 - self._number)
        if value:
            self._data[155] |= bit
        else:
            self._data[155] &= ~bit & 0x3F



class Patch:
    """One voice: six operators, the algorithm that wires them, and the rest."""

    def __init__(self):
        self._data = bytearray(_audiodsp.fm_init_voice())
        self._operators = tuple(Operator._of(self, n) for n in range(1, 7))

    @property
    def operators(self):
        """The six operators. ``operators[0]`` is operator 1."""
        return self._operators

    @property
    def algorithm(self):
        """How the six operators are wired, 1 to 32."""
        return self._data[134] + 1

    @algorithm.setter
    def algorithm(self, value):
        value = int(value)
        if value < 1 or value > 32:
            raise ValueError("algorithm must be 1 to 32")
        self._data[134] = value - 1

    @property
    def name(self):
        """Up to ten ASCII characters."""
        return "".join(chr(c) if 32 <= c < 127 else "?"
                       for c in self._data[145:155]).rstrip(" ")

    @name.setter
    def name(self, value):
        raw = str(value).encode("ascii")  # raises on anything else
        if len(raw) > 10:
            raise ValueError("a name is at most 10 characters")
        self._data[145:155] = raw + b" " * (10 - len(raw))


    def pack(self):
        """The voice as 155 bytes, in the single-voice parameter order."""
        return bytes(self._data[:_VOICE_BYTES])

    @staticmethod
    def unpack(data):
        """A `Patch` from 155 voice bytes, or from a 163-byte single-voice
        system exclusive message. Values past a parameter's range are
        clamped."""
        data = bytes(data)
        if len(data) == _VOICE_SYSEX:
            if (data[:2] != b"\xf0\x43" or data[2] & 0xF0 or data[3:6] != b"\x00\x01\x1b"
                    or data[-1] != 0xF7):
                raise ValueError("not a single-voice message")
            body = data[6:6 + _VOICE_BYTES]
            if _audiodsp.fm_checksum(body) != data[161]:
                raise ValueError("checksum does not match")
            data = body
        if len(data) != _VOICE_BYTES:
            raise ValueError("a voice is 155 bytes, or 163 as a message")
        patch = Patch()
        values = bytearray(data + b"\x3f")
        for index, top in enumerate(_MAX):
            if values[index] > top:
                values[index] = top
        patch._data[:] = values
        return patch

    @staticmethod
    def bank(data):
        """The 32 voices of a bank, as a list of `Patch`. Takes the 4104-byte
        system exclusive message, checked against its checksum, or its 4096
        data bytes alone."""
        data = bytes(data)
        if len(data) == _BANK_SYSEX:
            if (data[:2] != b"\xf0\x43" or data[2] & 0xF0 or data[3:6] != b"\x09\x20\x00"
                    or data[-1] != 0xF7):
                raise ValueError("not a 32-voice bank message")
            body = data[6:6 + _BANK_DATA]
            if _audiodsp.fm_checksum(body) != data[4102]:
                raise ValueError("checksum does not match")
            data = body
        if len(data) != _BANK_DATA:
            raise ValueError("a bank is 4104 bytes, or 4096 without its header")
        patches = []
        for index in range(32):
            patch = Patch()
            start = index * _PACKED_BYTES
            patch._data[:] = _audiodsp.fm_unpack_voice(data[start:start + _PACKED_BYTES])
            patches.append(patch)
        return patches

    @staticmethod
    def pack_bank(patches):
        """Up to 32 patches as a 4104-byte bank message. Missing slots are
        filled with the initial voice."""
        patches = list(patches)
        if len(patches) > 32:
            raise ValueError("a bank holds 32 voices")
        body = bytearray()
        for index in range(32):
            patch = patches[index] if index < len(patches) else Patch()
            if not isinstance(patch, Patch):
                raise TypeError("a bank holds Patch objects")
            body += _audiodsp.fm_pack_voice(bytes(patch._data))
        return (b"\xf0\x43\x00\x09\x20\x00" + bytes(body)
                + bytes((_audiodsp.fm_checksum(body), 0xF7)))


def _field(name, offset, kind, operator):
    if operator:
        def get(self):
            return _get(self._data, self._base + offset, kind)

        def put(self, value):
            _set(self._data, self._base + offset, kind, name, value)
    else:
        def get(self):
            return _get(self._data, offset, kind)

        def put(self, value):
            _set(self._data, offset, kind, name, value)
    return property(get, put)


# The fields become properties, so they show in dir() and in the stubs the
# way the MicroPython module's do.
for _name, (_offset, _kind) in _OP_FIELDS.items():
    setattr(Operator, _name, _field(_name, _offset, _kind, True))
for _name, (_offset, _kind) in _PATCH_FIELDS.items():
    setattr(Patch, _name, _field(_name, _offset, _kind, False))
del _name, _offset, _kind


class Synth(_AudioSample):
    """A polyphonic source that plays a `Patch`."""

    def __init__(self, voices=16, sample_rate=48000, channel_count=1, patch=None):
        voices = int(voices)
        if voices < 1 or voices > MAX_VOICES:
            raise ValueError("voices must be 1 to %d" % (MAX_VOICES,))
        channel_count = int(channel_count)
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        sample_rate = int(sample_rate)
        if sample_rate < 1000:
            raise ValueError("sample_rate must be at least 1000")
        self.sample_rate = sample_rate
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.max_buffer_length = FRAMES * 2 * channel_count
        self._deinited = False
        self._voices = voices
        self._state = _audiodsp.FMState(sample_rate=sample_rate,
                                        channel_count=channel_count, voices=voices)
        self._patch = Patch() if patch is None else patch
        self._check_patch(self._patch)
        self._bend = 0.0
        self._bend_range = 2
        self._mod_wheel = 0.0
        self._sustain = False

    @staticmethod
    def _check_patch(patch):
        if not isinstance(patch, Patch):
            raise TypeError("patch must be an audiofm.Patch")

    @property
    def voices(self):
        """How many notes can sound at once."""
        return self._voices

    @property
    def active(self):
        """How many voices are sounding, releases included."""
        self._check()
        return self._state.active()

    @property
    def patch(self):
        """The `Patch` new notes play. Edits to it apply from the next note."""
        return self._patch

    @patch.setter
    def patch(self, value):
        self._check()
        self._check_patch(value)
        self._patch = value

    def press(self, note, velocity=100):
        """Start `note` (0 to 127) at `velocity` (0 to 127)."""
        self._check()
        note = int(note)
        velocity = int(velocity)
        if note < 0 or note > 127:
            raise ValueError("note must be 0 to 127")
        if velocity < 0 or velocity > 127:
            raise ValueError("velocity must be 0 to 127")
        self._state.press(bytes(self._patch._data), note, velocity)

    def release(self, note):
        """Release every voice holding `note`."""
        self._check()
        note = int(note)
        if note < 0 or note > 127:
            raise ValueError("note must be 0 to 127")
        self._state.release(note)

    def release_all(self):
        """Release every held note, as if every key came up."""
        self._check()
        self._state.release_all()

    @property
    def sustain(self):
        """The sustain pedal. Lifting it releases the notes it held."""
        return self._sustain

    @sustain.setter
    def sustain(self, value):
        self._check()
        self._sustain = bool(value)
        self._state.set_sustain(self._sustain)

    def _apply_bend(self):
        steps = int(self._bend * 8192)
        steps = -8192 if steps < -8192 else 8191 if steps > 8191 else steps
        self._state.set_bend((steps * self._bend_range * 1398101) >> 13)

    @property
    def pitch_bend(self):
        """-1.0 to 1.0, scaled by `bend_range`."""
        return self._bend

    @pitch_bend.setter
    def pitch_bend(self, value):
        self._check()
        value = float(value)
        self._bend = -1.0 if value < -1.0 else 1.0 if value > 1.0 else value
        self._apply_bend()

    @property
    def bend_range(self):
        """How many semitones a full `pitch_bend` moves, 0 to 24."""
        return self._bend_range

    @bend_range.setter
    def bend_range(self, value):
        self._check()
        value = int(value)
        if value < 0 or value > 24:
            raise ValueError("bend_range must be 0 to 24")
        self._bend_range = value
        self._apply_bend()

    @property
    def mod_wheel(self):
        """0.0 to 1.0. Adds LFO pitch and amplitude depth to the patch's."""
        return self._mod_wheel

    @mod_wheel.setter
    def mod_wheel(self, value):
        self._check()
        value = float(value)
        self._mod_wheel = 0.0 if value < 0.0 else 1.0 if value > 1.0 else value
        self._state.set_mod_wheel(int(self._mod_wheel * 127 + 0.5))

    def _release(self):
        self._state.reset()

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        return GET_BUFFER_MORE_DATA, self._publish(self._state.render(FRAMES))


__all__ = ("Operator", "Patch", "Synth")
