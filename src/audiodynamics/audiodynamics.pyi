# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Compression, limiting, expansion, gating and transient shaping.

`Dynamics` is one node that does the job of a compressor, a limiter, an
expander, a gate or a transient shaper, chosen by its mode. Hand it an audio
source with `play`, and the result is another audio source. Its detector can
run on the signal itself or on a separate key signal given to `key`, which is
how a sidechain compressor is made.

Beyond the usual settings it has a number of options, every one of them off by
default, so a node that sets none of them does what a plain compressor does:
an RMS detector, a feedback detector and an external key input with Key
Listen, a 4x true-peak reconstruction, a two-pole key band with both corners
settable, a settable expander and gate depth, a four-stage gate envelope with
hold and hysteresis, a relative-threshold gain computer, a program-dependent
attack, and the transient shaper's four detector time constants, a second
envelope pair and a hold on the slow one.

A level moved on a playing node walks to its new value over 20 ms instead of
stepping, so turning ``makeup_db`` in one call does not click. Every option
changes together or not at all, so a bad keyword leaves the node as it was.

The node takes and gives 16-bit audio in blocks of 256 frames, with 1 or 2
channels, and never reports itself finished: a chain with nothing left to
read gets silence.
"""

from typing import Protocol, Self

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

DYN_COMPRESS: int
"""A mode: turn down what is above the threshold, by ``ratio``. This is 0."""
DYN_LIMIT: int
"""A mode: stop the level at the threshold. This is 1."""
DYN_EXPAND: int
"""A mode: turn down what is below the threshold, by ``ratio``. This is 2."""
DYN_GATE: int
"""A mode: shut off what is below the threshold. This is 3."""
DYN_TRANSIENT: int
"""A mode: reshape the start and the body of notes, by ``attack_gain_db`` and
``sustain_gain_db``. This is 4."""

class _AudioSample(Protocol):
    @property
    def sample_rate(self) -> int: ...
    @property
    def bits_per_sample(self) -> int: ...
    @property
    def channel_count(self) -> int: ...

class Dynamics:
    """A compressor, limiter, expander, gate or transient shaper.

    The mode is the one positional argument; everything else is a keyword.

    Args:
        mode: One of `DYN_COMPRESS`, `DYN_LIMIT`, `DYN_EXPAND`, `DYN_GATE`
            and `DYN_TRANSIENT`. The default is `DYN_COMPRESS`.
        sample_rate: Frames per second. The default is 48000.
        channel_count: 1 or 2. The default is 2.
        threshold_db: The level in dB where the effect starts, for
            every mode. The default is -24.
        ratio: The compression or expansion ratio, 1 or more (lower
            values are 1). The default is 4.
        knee_db: The width of the soft knee around the threshold, in
            dB, 0 or more. The default is 6.
        makeup_db: Gain added after the effect, in dB. A change walks
            to its new value over 20 ms. The default is 0.
        attack_ms: How fast the detector follows a rise, in
            milliseconds. The default is 10.
        release_ms: How fast the detector follows a fall, in
            milliseconds. The default is 120.
        attack_gain_db: `DYN_TRANSIENT` only: the gain in dB for the
            start of a note. The default is 0.
        sustain_gain_db: `DYN_TRANSIENT` only: the gain in dB for the
            body of a note. The default is 0.
        sidechain_hz: A high-pass corner in Hz on the signal the
            detector hears, so low notes do not pump the gain. 0 or
            less is off, the default.
        lookahead_ms: How far ahead of the output the detector looks,
            in milliseconds, 0 to 50. It adds that much latency. The
            default is 0.
        true_peak: How the detector finds peaks between samples. 0 or
            False reads the samples; 1 or True estimates the peak with
            a half-band filter; 2 or more reconstructs it at four
            times the rate. The default is 0.
        transient_fast_attack_ms: `DYN_TRANSIENT`: the fast envelope's
            attack time, in milliseconds. The default is 1.
        transient_fast_release_ms: `DYN_TRANSIENT`: the fast
            envelope's release time, in milliseconds. The default is
            50.
        transient_slow_attack_ms: `DYN_TRANSIENT`: the slow envelope's
            attack time, in milliseconds. The default is 25.
        transient_slow_release_ms: `DYN_TRANSIENT`: the slow
            envelope's release time, in milliseconds. The default is
            300.
        detector: What the detector measures: ``'peak'`` or ``'rms'``,
            or 0 or 1 for the same. The default is ``'peak'``.
        rms_ms: The averaging window of the RMS detector, in
            milliseconds. The default is 10.
        feedback_detector: Have the detector read the previous output
            frame instead of the input, the feedback arrangement of
            classic hardware compressors. The default is off.
        sidechain_lp_hz: A low-pass corner in Hz on the signal the
            detector hears, which with ``sidechain_hz`` makes a band.
            0 or less is off, the default.
        sidechain_poles: How steep the key band's filters are: 1 pole
            (6 dB an octave) or 2 (12). The default is 1.
        key_listen: Put the detector's signal on the output instead of
            the audio, as a gate's Key Listen switch does. The default
            is off.
        depth_db: The floor `DYN_EXPAND` and `DYN_GATE` pull the level
            down to, in dB, zero or below. A positive value, the
            default, leaves it unset: -60 dB for the expander and -80
            dB for the gate.
        hold_ms: `DYN_GATE`: how long the gate holds open after the
            level falls below the threshold, in milliseconds, which
            makes the gate's envelope four-stage. 0 is no hold, the
            default.
        hysteresis_db: `DYN_GATE`: the gap in dB between the level
            that opens the gate and the one that closes it. The
            default is 0.
        relative_threshold: Drive the gain computer by the key band's
            level minus the full-band level, so a fixed spectral
            balance gets the same reduction wherever it sits in the
            level range. The default is off.
        program_attack: Scale the attack by how far the level
            overshoots the envelope, so a bigger overshoot is caught
            faster. The default is off.
        transient_dual: `DYN_TRANSIENT`: apply the attack and sustain
            gains together, the sustain section reading its own,
            slower pair of envelopes. The default is off.
        sustain_fast_attack_ms: `DYN_TRANSIENT`, with
            ``transient_dual``: the sustain section's fast attack, in
            milliseconds. The default is 1.
        sustain_fast_release_ms: `DYN_TRANSIENT`, with
            ``transient_dual``: the sustain section's fast release, in
            milliseconds. The default is 200.
        sustain_slow_attack_ms: `DYN_TRANSIENT`, with
            ``transient_dual``: the sustain section's slow attack, in
            milliseconds. The default is 25.
        sustain_slow_release_ms: `DYN_TRANSIENT`, with
            ``transient_dual``: the sustain section's slow release, in
            milliseconds. The default is 1200.
        slow_hold_ms: `DYN_TRANSIENT`: how long the slow envelope
            holds its peak before it releases, in milliseconds. 0 is
            no hold, the default.
        gain_smooth_ms: Smooth the computed gain with a one-pole
            filter of this time, in milliseconds. 0 or less is off,
            the default.
        feedback_gain_corrected: With ``feedback_detector`` in
            `DYN_COMPRESS`, make the loop land on the ratio you asked
            for. Off, the loop reduces its own input and settles near
            2:1 whatever the ratio. The default is off.

    Raises:
        ValueError: ``channel_count`` is not 1 or 2, or ``detector`` is not
            ``'peak'`` or ``'rms'``.
        TypeError: An unknown option was given.
    """

    def __init__(
        self,
        mode: int = 0,
        *,
        sample_rate: int = 48000,
        channel_count: int = 2,
        threshold_db: float = ...,
        ratio: float = ...,
        knee_db: float = ...,
        makeup_db: float = ...,
        attack_ms: float = ...,
        release_ms: float = ...,
        attack_gain_db: float = ...,
        sustain_gain_db: float = ...,
        sidechain_hz: float = ...,
        lookahead_ms: float = ...,
        true_peak: float = ...,
        transient_fast_attack_ms: float = ...,
        transient_fast_release_ms: float = ...,
        transient_slow_attack_ms: float = ...,
        transient_slow_release_ms: float = ...,
        detector: str | float = ...,
        rms_ms: float = ...,
        feedback_detector: bool = ...,
        sidechain_lp_hz: float = ...,
        sidechain_poles: float = ...,
        key_listen: bool = ...,
        depth_db: float = ...,
        hold_ms: float = ...,
        hysteresis_db: float = ...,
        relative_threshold: bool = ...,
        program_attack: bool = ...,
        transient_dual: bool = ...,
        sustain_fast_attack_ms: float = ...,
        sustain_fast_release_ms: float = ...,
        sustain_slow_attack_ms: float = ...,
        sustain_slow_release_ms: float = ...,
        slow_hold_ms: float = ...,
        gain_smooth_ms: float = ...,
        feedback_gain_corrected: bool = ...,
    ) -> None: ...
    sample_rate: int
    """Frames per second. Writable, to 1 or more."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    def play(self, source: _AudioSample, /) -> None:
        """Set the stream to process, replacing the last one."""
    def key(self, source: _AudioSample | None, /) -> None:
        """Set the key signal the detector listens to instead of the stream
        itself, or None to go back to the stream. The key has to match the
        stream's channel count."""
    def set(
        self,
        *,
        sample_rate: int = ...,
        channel_count: int = ...,
        threshold_db: float = ...,
        ratio: float = ...,
        knee_db: float = ...,
        makeup_db: float = ...,
        attack_ms: float = ...,
        release_ms: float = ...,
        attack_gain_db: float = ...,
        sustain_gain_db: float = ...,
        sidechain_hz: float = ...,
        lookahead_ms: float = ...,
        true_peak: float = ...,
        transient_fast_attack_ms: float = ...,
        transient_fast_release_ms: float = ...,
        transient_slow_attack_ms: float = ...,
        transient_slow_release_ms: float = ...,
        detector: str | float = ...,
        rms_ms: float = ...,
        feedback_detector: bool = ...,
        sidechain_lp_hz: float = ...,
        sidechain_poles: float = ...,
        key_listen: bool = ...,
        depth_db: float = ...,
        hold_ms: float = ...,
        hysteresis_db: float = ...,
        relative_threshold: bool = ...,
        program_attack: bool = ...,
        transient_dual: bool = ...,
        sustain_fast_attack_ms: float = ...,
        sustain_fast_release_ms: float = ...,
        sustain_slow_attack_ms: float = ...,
        sustain_slow_release_ms: float = ...,
        slow_hold_ms: float = ...,
        gain_smooth_ms: float = ...,
        feedback_gain_corrected: bool = ...,
    ) -> None:
        """Change options while the node plays.

        Takes the same options as the constructor. All of them change together
        or not at all, and a change to ``lookahead_ms`` or ``channel_count``
        allocates what it needs before anything changes.

        Raises:
            ValueError: As for the constructor.
            TypeError: An unknown option was given.
        """
    def gain_reduction_db(self) -> float:
        """How much the node is turning the level down now, in dB, for a meter."""
    def deinit(self) -> None:
        """Release the node. Later calls other than `deinit` raise
        `ValueError`."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
