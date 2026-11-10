# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Recorded notes, played at any pitch.

`Sampler` is an audio source with a fixed number of voices. A voice plays a
recording of a note at any pitch, loops a region of it while the key is held,
and lets the rest out when the key lifts, under an amplitude envelope with an
optional resonant low-pass filter, a modulation envelope and two LFOs. That
is what a SoundFont instrument and a drum machine need, and what
`audiomixer.Mixer` (which plays sources at their own rate) and `synthio`
(which plays single-cycle waveforms) cannot do.

`SampleBuffer` holds samples in memory outside the collected heap, so a bank
of many megabytes does not slow every garbage collection.

All voices are mixed in one C call per block, through a 4-point Hermite
interpolator.
"""

from typing import Self

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

FRAMES: int
"""Frames in one block when ``buffer_size`` is not given: 256."""
MAX_FRAMES: int
"""The most frames in one block: 1024."""
MAX_VOICES: int
"""The most voices a `Sampler` can have: 128."""
LOOP_NONE: int
"""A loop mode: play to the end of the buffer and stop. This is SoundFont's
``sampleModes`` value 0."""
LOOP_FOREVER: int
"""A loop mode: loop ``[loop_start, loop_end)`` until `SamplerVoice.release`
or `SamplerVoice.stop`. This is SoundFont's ``sampleModes`` value 1."""
LOOP_UNTIL_RELEASE: int
"""A loop mode: loop while the key is held, then play on past the loop to the
end of the buffer. This is SoundFont's ``sampleModes`` value 3."""

class SampleBuffer:
    """Zeroed memory for samples, outside the collected heap.

    Read audio into it, or copy it in, and play views of it with
    `SamplerVoice.play`. The memory comes from PSRAM on boards whose audio
    driver says where, and from the system allocator elsewhere.

    A `memoryview` of a buffer does not keep it alive. Hold the buffer itself
    for as long as a voice plays from it.

    Args:
        size: Bytes to allocate and zero.

    Raises:
        ValueError: ``size`` is negative.
        MemoryError: The bytes could not be allocated.
    """

    def __init__(self, size: int, /) -> None: ...
    def __len__(self) -> int:
        """The size in bytes, which is 0 after `deinit`."""
    def __buffer__(self, flags: int, /) -> memoryview: ...
    def deinit(self) -> None:
        """Free the bytes at once. The buffer then has a length of 0."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class SamplerVoice:
    """One voice of a `Sampler`.

    Get voices from `Sampler.voice`; they cannot be made on their own. A voice
    plays one note at a time. Starting a new note on a sounding voice fades the
    old one out over 5 ms.
    """

    level: float
    """The voice's level, where 1.0 is unity gain. It is limited to 0.0 to 1.0
    when applied, and reads back as you set it. A live setting: the change lands at the next block, as a ramp across it, so a
    fader move does not click."""
    pan: float
    """Position in the stereo field from -1.0 (left) to 1.0 (right), limited to
    that range when applied. A live setting, ramped like `level`."""
    pitch: float
    """Playback speed as a ratio, 1.0 for the recording's own pitch, from 1/64
    to 64 (clamped). A live setting, which is how a pitch bend is played."""
    cutoff: float | None
    """The low-pass filter's cutoff in Hz, or None for no filter. A live
    setting: a change lands within 32 frames, and a filter switched on starts
    from rest."""
    vibrato_depth: float
    """The vibrato LFO's depth in cents. A live setting, so a mod wheel is a
    series of writes here; each lands within 32 frames."""
    @property
    def playing(self) -> bool:
        """Whether the voice still sounds."""
    def play(
        self,
        buffer: bytes | bytearray | memoryview | SampleBuffer,
        *,
        rate: float | None = None,
        pitch: float | None = None,
        loop_start: int = 0,
        loop_end: int | None = None,
        loop_mode: int = 0,
        level: float | None = None,
        pan: float | None = None,
        delay: float | None = None,
        attack: float | None = None,
        hold: float | None = None,
        decay: float | None = None,
        sustain: float | None = None,
        release: float | None = None,
        cutoff: float | None = None,
        resonance: float | None = None,
        mod_delay: float | None = None,
        mod_attack: float | None = None,
        mod_hold: float | None = None,
        mod_decay: float | None = None,
        mod_sustain: float | None = None,
        mod_release: float | None = None,
        mod_to_pitch: float | None = None,
        mod_to_cutoff: float | None = None,
        vibrato_delay: float | None = None,
        vibrato_rate: float | None = None,
        vibrato_depth: float | None = None,
        lfo_delay: float | None = None,
        lfo_rate: float | None = None,
        lfo_to_pitch: float | None = None,
        lfo_to_cutoff: float | None = None,
        lfo_to_volume: float | None = None,
    ) -> None:
        """Start a note.

        The buffer is held for as long as the voice sounds and let go when it
        stops. The step per output frame is ``pitch * rate / sample_rate``,
        so a 44.1 kHz recording plays at the right speed through a 48 kHz
        output without being told anything else.

        Every option except ``buffer`` is a keyword, and None (the default)
        means the value given here.

        Args:
            buffer: The recording: mono signed 16-bit samples, in a buffer
                holding at least one whole sample.
            rate: The recording's own sample rate in Hz. The default is the
                sampler's.
            pitch: Playback speed as a ratio. The default is 1.0.
            loop_start: The first frame of the loop.
            loop_end: The frame after the last one of the loop. The default is
                the end of the buffer.
            loop_mode: `LOOP_NONE`, `LOOP_FOREVER` or `LOOP_UNTIL_RELEASE`.
            level: Level from 0.0 to 1.0, where 1.0 is unity gain. The default
                is 1.0.
            pan: Stereo position, -1.0 to 1.0. The default is 0.0.
            delay: Seconds before the sound starts, which is what a flam
                wants. The default is 0.0.
            attack: Amplitude envelope attack time in seconds. The default
                is 0.0.
            hold: Amplitude envelope hold time in seconds. The default is
                0.0.
            decay: Amplitude envelope decay time in seconds. The default is
                0.0.
            sustain: Amplitude envelope sustain level, linear. The default
                is 1.0.
            release: Amplitude envelope release time in seconds. The default
                is 0.0.
            cutoff: Low-pass cutoff in Hz. A cutoff of 19.9 kHz or more, with
                no resonance and nothing moving it, is no filter at all, and
                so is leaving it out.
            resonance: The filter's response at the cutoff above its response
                at DC, in dB, from 0 to 96. DC drops by half of it, as the
                SoundFont specification puts it. 0 is a Butterworth.
            mod_delay: The modulation envelope's delay in seconds. This
                envelope has the amplitude envelope's six stages and moves
                pitch and cutoff. The default is 0.0.
            mod_attack: Its attack time in seconds. The default is 0.0.
            mod_hold: Its hold time in seconds. The default is 0.0.
            mod_decay: Its decay time in seconds. The default is 0.0.
            mod_sustain: Its sustain level, linear. The default is 1.0.
            mod_release: Its release time in seconds. The default is 0.0.
            mod_to_pitch: How far the modulation envelope moves the pitch at
                full scale, in cents. The default is 0.0.
            mod_to_cutoff: How far it moves the cutoff at full scale, in
                cents. The default is 0.0.
            vibrato_delay: Seconds before the vibrato LFO starts. The default
                is 0.0.
            vibrato_rate: The vibrato LFO's rate in Hz. The default is 0.0.
            vibrato_depth: The vibrato LFO's depth in cents, moving the
                pitch. The default is 0.0.
            lfo_delay: Seconds before the modulation LFO starts. The default
                is 0.0.
            lfo_rate: The modulation LFO's rate in Hz. The default is 0.0.
            lfo_to_pitch: How far the modulation LFO moves the pitch at its
                peak, in cents. The default is 0.0.
            lfo_to_cutoff: How far it moves the cutoff at its peak, in cents.
                The default is 0.0.
            lfo_to_volume: How far it moves the level at its peak, in dB. The
                default is 0.0.

        Raises:
            ValueError: ``buffer`` holds less than one whole 16-bit sample,
                ``loop_start`` or ``loop_end`` is negative, ``loop_mode`` is
                not one of the three, the loop does not fit the buffer, or the
                sampler was deinitialised.
        """
    def release(self) -> None:
        """Let the key go: enter the release stage from wherever the envelope
        is. In `LOOP_UNTIL_RELEASE` the voice also stops looping and plays on
        to the end."""
    def stop(self) -> None:
        """Free the voice at once. The note fades out over 5 ms rather than
        being cut."""
    def deinit(self) -> None:
        """Stop the voice with no fade and let go of its buffer. The voice
        belongs to its sampler, which keeps working."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...

class Sampler:
    """A bank of sample-playing voices, played like any audio sample.

    The output is signed 16-bit. Notes start on `voice`.

    Args:
        voice_count: How many voices, 1 to `MAX_VOICES`.
        buffer_size: Bytes in one block, a whole number of frames up to
            `MAX_FRAMES`. 0 means `FRAMES` frames.
        channel_count: 1 or 2.
        bits_per_sample: Only 16 is accepted. The argument exists so a sampler
            can stand where a mixer stood.
        samples_signed: Only True is accepted, for the same reason.
        sample_rate: Frames per second, 1 or more.

    Raises:
        ValueError: An argument is out of range, or ``bits_per_sample`` or
            ``samples_signed`` asks for anything but signed 16-bit.
    """

    def __init__(
        self,
        *,
        voice_count: int = 8,
        buffer_size: int = 0,
        channel_count: int = 2,
        bits_per_sample: int = 16,
        samples_signed: bool = True,
        sample_rate: int = 48000,
    ) -> None: ...
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    @property
    def voice(self) -> tuple[SamplerVoice, ...]:
        """The voices, as a tuple of `voice_count` `SamplerVoice` objects."""
    @property
    def voice_count(self) -> int:
        """How many voices the sampler has."""
    @property
    def playing(self) -> bool:
        """Whether any voice sounds, a stopped note's fade included."""
    def stop(self) -> None:
        """Stop every voice, each with the 5 ms fade of `SamplerVoice.stop`."""
    def deinit(self) -> None:
        """Stop every voice and let go of every buffer. Every later use raises
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
