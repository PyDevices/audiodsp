# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Multi-channel audio synthesis.

This is CircuitPython's own `synthio` API, so one script plays on both. A
`Synthesizer` plays `Note` objects, or plain MIDI note numbers, through an
`Envelope` and a waveform. A `Note` can take a `Biquad` filter, a ring
modulator, and a `BlockInput` on its amplitude, pitch bend and panning: a
number, an `LFO` or a `Math` block that is advanced while the synthesizer
plays. A `MidiTrack` plays a stream of MIDI note events.

Beyond CircuitPython's own API this build adds `lfo_tick`,
`Synthesizer.max_polyphony`, `Synthesizer.refused` and
`Synthesizer.release_then_press`.
"""

from array import array
from typing import BinaryIO, Protocol, Self, Sequence, Union

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

# Types the API takes. They are for the type checker and are not importable
# from the module.
_ReadableBuffer = Union[bytes, bytearray, memoryview, array[int]]
_BlockInput = Union["Math", "LFO", float, None]
_NoteSequence = Sequence[Union[int, "Note"]]
_NoteOrNoteSequence = Union[int, "Note", _NoteSequence]
_LFOOrLFOSequence = Union["LFO", Sequence["LFO"]]

waveform_max_length: int
"""The most samples a waveform can have: 16384. Pass it as a loop end to loop
to the end of a waveform whatever its length."""

class EnvelopeState:
    """Which phase of its envelope a note is in."""

    ATTACK: EnvelopeState
    """The note is in its attack phase."""
    DECAY: EnvelopeState
    """The note is in its decay phase."""
    SUSTAIN: EnvelopeState
    """The note is in its sustain phase."""
    RELEASE: EnvelopeState
    """The note is in its release phase."""

class Envelope(tuple[float, float, float, float, float]):
    """An ADSR envelope with linear ramps.

    A note starts at zero volume, rises to ``attack_level`` over
    ``attack_time`` seconds, falls to ``sustain_level`` over ``decay_time``
    seconds, and when it is released falls to zero over ``release_time``.

    If ``sustain_level`` is 0, the decay and sustain phases are omitted and the
    note counts as released as soon as the attack ends, as a plucked or struck
    instrument behaves. ``decay_time`` is then ignored.

    If a note is released before its sustain phase, it falls with the slope of
    ``sustain_level / release_time`` (or ``attack_level / release_time`` for a
    plucked envelope).

    An envelope is an immutable tuple of its five values, in the order of the
    arguments.

    Args:
        attack_time: Seconds to ramp from 0 to ``attack_level``. The default
            is 0.1. It cannot be negative.
        decay_time: Seconds to ramp from ``attack_level`` to
            ``sustain_level``. The default is 0.05. It cannot be negative.
        release_time: Seconds to ramp from the current level to 0 on release.
            The default is 0.2. It cannot be negative.
        attack_level: The peak level of the attack, from 0.0 to 1.0. The
            default is 1.0.
        sustain_level: The level of the sustain phase relative to the attack
            level, from 0.0 to 1.0. The default is 0.8.

    Raises:
        ValueError: A time is negative, or a level is outside 0.0 to 1.0.
    """

    def __new__(
        cls,
        *,
        attack_time: float = 0.1,
        decay_time: float = 0.05,
        release_time: float = 0.2,
        attack_level: float = 1.0,
        sustain_level: float = 0.8,
    ) -> Self: ...
    @property
    def attack_time(self) -> float:
        """Seconds to ramp from 0 volume to the attack level."""
    @property
    def decay_time(self) -> float:
        """Seconds to ramp from the attack level to the sustain level."""
    @property
    def release_time(self) -> float:
        """Seconds to ramp down on release."""
    @property
    def attack_level(self) -> float:
        """The peak level of the attack phase, from 0.0 to 1.0."""
    @property
    def sustain_level(self) -> float:
        """The level of the sustain phase relative to the attack level, from
        0.0 to 1.0."""

class FilterMode:
    """The type of a `Biquad` filter."""

    LOW_PASS: FilterMode
    """A low-pass filter."""
    HIGH_PASS: FilterMode
    """A high-pass filter."""
    BAND_PASS: FilterMode
    """A band-pass filter."""
    NOTCH: FilterMode
    """A notch filter."""
    LOW_SHELF: FilterMode
    """A low shelf filter."""
    HIGH_SHELF: FilterMode
    """A high shelf filter."""
    PEAKING_EQ: FilterMode
    """A peaking equalizer filter."""

class Biquad:
    """A biquad filter, evaluated as a direct form 1 filter.

    ``frequency`` is the centre or corner frequency in Hz, depending on the
    mode. ``Q`` is the sharpness. ``A`` sets the gain of peaking and shelving
    filters as ``A = 10 ** (dB_gain / 40)`` and is ignored by the others.

    Because ``frequency``, ``Q`` and ``A`` are block inputs, they can vary
    while the note plays. The filter's internal state is not updated when the
    coefficients change; in practice, varying them slowly behaves as you would
    expect.

    Args:
        mode: The filter type.
        frequency: The centre or corner frequency in Hz.
        Q: The sharpness. The default is 0.7071067811865475.
        A: The gain of peaking and shelving filters. The default is None.
    """

    def __init__(
        self,
        mode: FilterMode,
        frequency: _BlockInput,
        Q: _BlockInput = ...,
        A: _BlockInput = None,
    ) -> None: ...
    @property
    def mode(self) -> FilterMode:
        """The type of filter. It cannot be changed."""
    frequency: _BlockInput
    """The centre or corner frequency of the filter in Hz."""
    Q: _BlockInput
    """The sharpness of the filter."""
    A: _BlockInput
    """The gain of a peaking or shelving filter, ``10 ** (dB_gain / 40)``."""

class LFO:
    """A low-frequency oscillator block.

    Every ``rate`` seconds the LFO cycles through its ``waveform``. The output
    at any moment is ``waveform[index] * scale + offset``. The waveform's
    ``'h'`` values are scaled so that -32768..32767 maps to -1.0..0.99996.
    With no waveform, a triangle is used.

    An LFO only updates while it is tied to a playing `Synthesizer`, directly,
    through a `Note`, or through another block, and then it updates every 256
    samples. Using one LFO as the input of several blocks is fine; tying it to
    several synthesizers is undefined. The ``value`` is worked out when the LFO
    is made and whenever its synthesizer updates it, so changing ``offset``
    alone does not change ``value``.

    ``once`` changes how the waveform is interpolated. With
    ``waveform=array('h', [0, 100])`` and ``rate=1``, ``once=True`` ramps from 0
    to 100 over one second and stays there, while ``once=False`` is a triangle
    with a one second period that peaks at half a second. Changing ``once``
    other than at a phase of 0 steps the output.

    Args:
        waveform: A buffer of signed 16-bit samples, or None for a triangle.
            Its values may be changed while it is in use.
        rate: Cycles per second.
        scale: A multiplier on the output.
        offset: An addend on the output.
        phase_offset: An addend on the phase, in cycles.
        once: True to stop at the last output value, False to start again.
        interpolate: True to interpolate linearly between values.
    """

    def __init__(
        self,
        waveform: _ReadableBuffer | None = None,
        *,
        rate: _BlockInput = 1.0,
        scale: _BlockInput = 1.0,
        offset: _BlockInput = 0.0,
        phase_offset: _BlockInput = 0.0,
        once: bool = False,
        interpolate: bool = True,
    ) -> None: ...
    @property
    def waveform(self) -> _ReadableBuffer | None:
        """The waveform. It cannot be replaced, but its values may change."""
    rate: _BlockInput
    """The rate in Hz at which the LFO cycles through its waveform."""
    offset: _BlockInput
    """An addend on the LFO's output."""
    phase_offset: _BlockInput
    """An addend on the LFO's phase."""
    scale: _BlockInput
    """A multiplier on the LFO's output."""
    once: bool
    """True if the waveform stops at its last output value, False if it starts
    again. This applies to the phase before ``phase_offset`` is added."""
    interpolate: bool
    """True if the waveform is interpolated linearly between values."""
    @property
    def phase(self) -> float:
        """The phase of the oscillator, from 0 to 1."""
    @property
    def value(self) -> float:
        """The value of the oscillator."""
    def retrigger(self) -> None:
        """Reset the LFO to the start of its waveform. It is most useful when
        ``once`` is True."""

class MathOperation:
    """The operation of a `Math` block.

    Calling an operation, as in ``MathOperation.SUM(a, b)``, makes a `Math` block
    that performs it.
    """

    def __call__(self, a: _BlockInput, b: _BlockInput = 0.0, c: _BlockInput = 1.0) -> Math:
        """Make a `Math` block that performs this operation."""
    SUM: MathOperation
    """``a + b + c``. For a two input sum, set one argument to 0.0. To hold a
    control value for several subscribers, set two arguments to 0.0."""
    ADD_SUB: MathOperation
    """``a + b - c``. For a two input subtraction, set ``b`` to 0.0."""
    PRODUCT: MathOperation
    """``a * b * c``. For a two input product, set one argument to 1.0."""
    MUL_DIV: MathOperation
    """``a * b / c``. If ``c`` is zero, the output is 1.0."""
    SCALE_OFFSET: MathOperation
    """``(a * b) + c``."""
    OFFSET_SCALE: MathOperation
    """``(a + b) * c``. For a two input multiplication, set ``b`` to 0."""
    LERP: MathOperation
    """``a * (1 - c) + b * c``."""
    CONSTRAINED_LERP: MathOperation
    """``a * (1 - c') + b * c'``, where ``c'`` is limited to 0.0 to 1.0."""
    DIV_ADD: MathOperation
    """``a / b + c``. If ``b`` is zero, the output is ``c``."""
    ADD_DIV: MathOperation
    """``(a + b) / c``. For a two input quotient, set ``b`` to 0.0."""
    MID: MathOperation
    """The middle of the three inputs."""
    MAX: MathOperation
    """The biggest of the three inputs."""
    MIN: MathOperation
    """The smallest of the three inputs."""
    ABS: MathOperation
    """The absolute value of ``a``."""

class Math:
    """An arithmetic block that works on up to three inputs.

    Like an `LFO`, it only updates while it is tied to a playing `Synthesizer`,
    directly or through another block, and then every 256 samples. Its
    properties can change while it plays.

    Args:
        operation: What to compute.
        a: The first input.
        b: The second input.
        c: The third input.
    """

    def __init__(
        self,
        operation: MathOperation,
        a: _BlockInput,
        b: _BlockInput = 0.0,
        c: _BlockInput = 1.0,
    ) -> None: ...
    a: _BlockInput
    """The first input."""
    b: _BlockInput
    """The second input."""
    c: _BlockInput
    """The third input."""
    operation: MathOperation
    """The function to compute."""
    @property
    def value(self) -> float:
        """The result of the last update."""

class Note:
    """One note: a frequency, with optional waveform, envelope, filter, ring
    modulation, tremolo and bend.

    If ``waveform`` or ``envelope`` is None, the synthesizer's default is used.
    Playing one `Note` on several synthesizers is undefined.

    Args:
        frequency: The base frequency in Hz.
        panning: Where the note sits between the channels: -1 is left only, 0
            is both and 1 is right only. A fractional value plays the nearer
            channel at full amplitude and the other at part of it.
        amplitude: The relative amplitude from 0 to 1, multiplied with the
            envelope. Attach an `LFO` here for tremolo.
        bend: The pitch bend in octaves, from -12 to 12. A bend of 1/12 is a
            semitone. Attach an `LFO` here for vibrato or a sweep.
        waveform: A single cycle of signed 16-bit samples. The default is a
            square wave.
        waveform_loop_start: The index where looping begins, from 0 to
            ``len(waveform) - 1``.
        waveform_loop_end: The index where looping ends, from
            ``waveform_loop_start + 1`` to ``len(waveform)``. Use
            `waveform_max_length` to loop to the end whatever its length.
        envelope: The note's envelope.
        filter: A filter for the note's output. Make it with a filter-making
            method for the synthesizer that will play the note, because
            coefficients depend on the sample rate.
        ring_frequency: The ring modulator's frequency in Hz. 0 turns ring
            modulation off. Both this and ``ring_waveform`` must be set for
            it to take effect.
        ring_bend: The pitch bend of the ring waveform, in octaves, from -12
            to 12.
        ring_waveform: The ring modulator's waveform.
        ring_waveform_loop_start: As ``waveform_loop_start``, for the ring
            waveform.
        ring_waveform_loop_end: As ``waveform_loop_end``, for the ring
            waveform.
    """

    def __init__(
        self,
        frequency: float,
        *,
        panning: _BlockInput = 0.0,
        amplitude: _BlockInput = 1.0,
        bend: _BlockInput = 0.0,
        waveform: _ReadableBuffer | None = None,
        waveform_loop_start: _BlockInput = 0,
        waveform_loop_end: _BlockInput = ...,
        envelope: Envelope | None = None,
        filter: Biquad | None = None,
        ring_frequency: float = 0.0,
        ring_bend: float = 0.0,
        ring_waveform: _ReadableBuffer | None = None,
        ring_waveform_loop_start: _BlockInput = 0,
        ring_waveform_loop_end: _BlockInput = ...,
    ) -> None: ...
    frequency: float
    """The base frequency of the note in Hz."""
    filter: Biquad | None
    """The filter applied to this note's output, or None."""
    panning: _BlockInput
    """Where the note sits between the channels, from -1 (left) to 1 (right)."""
    amplitude: _BlockInput
    """The relative amplitude from 0 to 1, multiplied with the envelope."""
    bend: _BlockInput
    """The pitch bend in octaves, from -12 to 12."""
    waveform: _ReadableBuffer | None
    """The note's waveform. Setting a buffer of a different size resets the
    note's phase."""
    waveform_loop_start: _BlockInput
    """The index where looping begins, from 0 to ``len(waveform) - 1``."""
    waveform_loop_end: _BlockInput
    """The index where looping ends, from ``waveform_loop_start + 1`` to
    ``len(waveform)``."""
    envelope: Envelope | None
    """The note's envelope, or None for the synthesizer's."""
    ring_frequency: float
    """The ring modulator's frequency in Hz. 0 turns it off."""
    ring_bend: float
    """The pitch bend of the ring waveform in octaves, from -12 to 12."""
    ring_waveform: _ReadableBuffer | None
    """The ring modulator's waveform. Setting a buffer of a different size
    resets the note's phase."""
    ring_waveform_loop_start: _BlockInput
    """The index where looping of the ring waveform begins."""
    ring_waveform_loop_end: _BlockInput
    """The index where looping of the ring waveform ends."""

class Synthesizer:
    """A polyphonic synthesizer, played like any audio sample.

    Notes are `Note` objects or MIDI note numbers from 0 to 127, where 60 is
    middle C at about 262 Hz. A note given as a number uses the synthesizer's
    waveform and envelope and has no tremolo or vibrato.

    Args:
        sample_rate: Frames per second, 1 or more. A higher rate takes more
            memory.
        channel_count: 1 or 2.
        waveform: A single cycle of signed 16-bit samples. The default is a
            square wave with a 50% duty cycle.
        envelope: The envelope for notes that have none. The default of None
            turns notes on and off instantly.

    Raises:
        ValueError: ``sample_rate`` is below 1 or ``channel_count`` is out of
            range.
    """

    def __init__(
        self,
        *,
        sample_rate: int = 11025,
        channel_count: int = 1,
        waveform: _ReadableBuffer | None = None,
        envelope: Envelope | None = None,
    ) -> None: ...
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    envelope: Envelope | None
    """The envelope for all notes that have none. It can change while notes
    play, and then affects notes already playing."""
    @property
    def pressed(self) -> _NoteSequence:
        """The notes currently pressed. Notes in the release phase are not
        included."""
    @property
    def blocks(self) -> list[_BlockInput]:
        """Blocks to advance whether or not a playing note uses them, for
        free-running LFOs. The list cannot be replaced, but you can add to it
        and remove from it. It starts empty."""
    @property
    def max_polyphony(self) -> int:
        """How many notes can sound at once."""
    @property
    def refused(self) -> int:
        """How many presses were dropped because every channel was held. There
        is no stealing policy; this only counts. It includes presses that
        arrive through `audiopump.Events`, and is not reset by `release_all`."""
    def press(self, press: _NoteOrNoteSequence = ()) -> None:
        """Turn some notes on. Pressing a note that is already pressed has no
        effect.

        Args:
            press: A note or a sequence of notes.
        """
    def release(self, release: _NoteOrNoteSequence = ()) -> None:
        """Turn some notes off. Releasing a note that is not pressed has no
        effect.

        Args:
            release: A note or a sequence of notes.
        """
    def change(
        self,
        release: _NoteOrNoteSequence = (),
        press: _NoteOrNoteSequence = (),
        retrigger: _LFOOrLFOSequence = (),
    ) -> None:
        """Start notes, stop them and retrigger LFOs, all at once with respect
        to output generation.

        Pressing a note that is already pressed returns it to the attack phase
        without resetting its amplitude. Releasing a note and pressing it again
        in the same call returns it to the attack phase from an amplitude of 0.

        Args:
            release: Notes to turn off.
            press: Notes to turn on.
            retrigger: LFOs to retrigger.
        """
    def release_then_press(
        self,
        release: _NoteOrNoteSequence = (),
        press: _NoteOrNoteSequence = (),
        retrigger: _LFOOrLFOSequence = (),
    ) -> None:
        """Another name for `change`."""
    def release_all_then_press(self, press: _NoteOrNoteSequence) -> None:
        """Turn off every playing note, then turn on the given notes. A note
        that was playing returns to the attack phase from an amplitude of 0.

        Args:
            press: A note or a sequence of notes.
        """
    def release_all(self) -> None:
        """Turn off every playing note."""
    def note_info(self, note: Note) -> tuple[EnvelopeState | None, float]:
        """Report a note's envelope state.

        Returns:
            ``(state, level)`` for a note that is playing here, including one in
            its release phase, and ``(None, 0.0)`` for one that is not.
        """
    def deinit(self) -> None:
        """Release the synthesizer. Every later use raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class MidiTrack:
    """Plays a stream of MIDI note events, as an audio sample.

    Only note on and note off events are used; channels and velocities are
    ignored. Up to two notes may sound at once.

    Args:
        buffer: The events, as stored in a MIDI file's track chunk.
        tempo: Ticks per second of the events.
        sample_rate: Frames per second. A higher rate takes more memory.
        waveform: A single cycle of signed 16-bit samples. The default is a
            square wave with a 50% duty cycle.
        envelope: The envelope for notes. The default turns notes on and off
            instantly.
    """

    def __init__(
        self,
        buffer: _ReadableBuffer,
        tempo: int,
        *,
        sample_rate: int = 11025,
        waveform: _ReadableBuffer | None = None,
        envelope: Envelope | None = None,
    ) -> None: ...
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1."""
    tempo: int
    """Ticks per second of the events."""
    @property
    def error_location(self) -> int | None:
        """The offset in bytes within the MIDI data of a decoding error, or
        None."""
    def deinit(self) -> None:
        """Release the track. Every later use raises `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

def from_file(
    file: str | BinaryIO,
    *,
    sample_rate: int = 11025,
    waveform: _ReadableBuffer | None = None,
    envelope: Envelope | None = None,
) -> MidiTrack:
    """Make a `MidiTrack` from a MIDI file. Only single-track files (type 0)
    are supported.

    Args:
        file: A path, or a file already opened for reading in binary mode.
        sample_rate: Frames per second. A higher rate takes more memory.
        waveform: A single cycle of signed 16-bit samples. The default is a
            square wave with a 50% duty cycle.
        envelope: The envelope for notes. The default turns notes on and off
            instantly.
    """

def midi_to_hz(midi_note: float) -> float:
    """Convert a MIDI note number to Hz, where 60 is middle C and 69 is concert
    A."""

def voct_to_hz(ctrl: float) -> float:
    """Convert a 1 volt per octave signal to Hz, where 2.0 (24/12) is middle C
    and 2.75 (33/12) is concert A.

    Raises:
        ValueError: ``ctrl`` is outside -11 to 11.
    """

def lfo_tick(*blocks: _BlockInput) -> tuple[float, ...]:
    """Advance the LFO and math blocks one update and read them.

    Args:
        blocks: Blocks, or plain numbers.

    Returns:
        Each block's value after the update, in order.
    """
