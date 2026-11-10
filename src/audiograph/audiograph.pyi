# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""A whole patch rendered by one C call per block.

`Graph` renders a patch in one C call per block. You build it once, from nodes
that call the kernels audiodsp already has (the oscillator, the ladder, the
float biquad, the feedback delay, the all-pass cascade, the sample-and-hold)
and a few small ones a voice needs (an envelope, an LFO, noise, a mixer, a
VCA, a glide, a clipping two-pole filter, an overdrive and more), then call
`Graph.run` once a block. Nothing in Python runs between the nodes, which is
what lets a six-voice polysynth play in real time on an ESP32-P4. It is the
same C on CPython, MicroPython and CircuitPython, so a patch built and
listened to on the desktop renders the same samples on the board.

Example::

    import audiograph as ag

    g = ag.Graph(sample_rate=48000, frames=128, channel_count=2)
    pitch, gate, env, saw, vcf, out = (g.buffer() for _ in range(6))

    g.add(ag.CONST, 0, (), pitch)               # the key, in octaves from C4
    key = g.add(ag.CONST, 0, (), gate)
    g.add(ag.ENV, 0, (gate,), env)
    osc = g.add(ag.OSC, 0, (pitch, None, None, None), saw)
    g.set(osc, ag.OSC_WAVEFORM, 1)              # a sawtooth
    lad = g.add(ag.LADDER, 0, (saw, env), vcf)  # the envelope sweeps the cutoff
    g.set(lad, ag.LADDER_CUTOFF_HZ, 600.0)
    g.add(ag.VCA, 0, (vcf, env, None), out)
    g.add(ag.OUT, ag.ALWAYS, (out, out), None)

    pcm = bytearray(128 * 2 * 2)
    g.set(key, 0, ag.UNIT)                      # key down
    sounding = g.run(pcm, 0)                    # one block into pcm

A graph owns ``buffers`` blocks of ``frames`` signed 16-bit samples and a
table of up to ``nodes`` nodes. Every node reads some buffers, writes one,
belongs to a group and holds one kernel's settings behind up to sixteen float
parameters. Nodes run in the order you added them, so add a node after the
nodes it reads. Every node is mono; a stereo patch has two buffers where it
needs them. A block is 256 frames at most, a node has at most eight inputs
and a group is 0 to 31 or `ALWAYS`. A graph refuses a node with nothing to
write (every kind but `OUT` writes a buffer), an input or output id past its
buffers, and a node past its table.

The buffers carry one wire: `UNIT` counts is 1.0, the +10 V of a modular rack,
so audio at 0.5 is a quarter of full scale.

An input left None is open: an oscillator without a pitch input plays
`OSC_FREQUENCY`, a ladder without a cutoff input sits at `LADDER_CUTOFF_HZ`.
The kernel nodes take their parameters under the same numbers as the modules
that wrap them (`audiooscillator`, `audioladder`, `audiobiquad`, `audioecho`,
and `audiobiquad.AllPass` for `PHASER`), re-exported here so a caller needs
no other module.

Every node kind and every parameter index is a constant below; a kind's
parameters carry its name as a prefix.
"""

from typing import Self, Sequence

__version__: str
"""The audiodsp release this module came from."""
__revision__: str
"""Which source this build was made from, from `git describe`: a release tag,
or the nearest one and the commit. "unknown" when the build had no git."""

# Node kinds, the first argument of Graph.add

CONST: int
"""A node kind with no inputs: writes the value of parameter 0, in counts, every sample."""
OSC: int
"""A band-limited oscillator, `audiooscillator`'s. Inputs: pitch, fm, pwm, sync."""
LADDER: int
"""A transistor ladder filter, `audioladder`'s, with the cutoff input read every sample. Inputs: audio, cutoff."""
BIQUAD: int
"""A float biquad, `audiobiquad.Biquad`'s, which walks to new coefficients. Input: audio."""
FBDELAY: int
"""A feedback delay, `audioecho.FeedbackDelay`'s. Built with ``arg`` the line's length in milliseconds. `Graph.table` sets its wow shape. Input: audio."""
ENV: int
"""An ADSR envelope from 0 to `UNIT`; each stage is a one-pole approach, the attack aimed past 1.0 so it arrives with a slope. Input: gate."""
LFO: int
"""A low-frequency oscillator of one of five shapes, swinging plus and minus its amplitude. No inputs."""
NOISE: int
"""White noise. Parameter 0 is the amplitude in counts. No inputs."""
MIX: int
"""A mixer of up to eight inputs. Parameters 0 to 7 are a gain per input, and `MIX_OFFSET` adds an offset. A gain that moved ramps across the block."""
VCA: int
"""A voltage-controlled amplifier: audio times the chosen control input over `UNIT`, times the gain. Inputs: audio, cv_a, cv_b."""
OUT: int
"""Writes the run's output buffer: interleaved stereo, or the mean when the graph is mono. Parameter 0 is a scale. Inputs: left, right. It writes no buffer, so its output is None."""
PHASER: int
"""`audiobiquad.AllPass` on one channel: first-order all-pass stages, one feedback around the chain, and a dry and wet mix. Built with ``arg`` the number of stages (0 means 4). Input: audio."""
LAG: int
"""Glides a note's pitch CV to each new note, with the bend (pitch minus note) on top. Inputs: note, pitch, legato."""
SAMPLE_HOLD: int
"""Holds its signal at each rising edge of the clock, or `SH_RATE` times a second with the clock open. Inputs: signal, clock."""
TWOPOLE: int
"""A 12 dB per octave filter with its nonlinearity inside the feedback loop. Inputs: audio, cutoff CV."""
OVERDRIVE: int
"""`audiofilters.Distortion`'s soft-clipped overdrive followed by a high shelf at 1.5 kHz. CircuitPython's own `audiograph` lacks it; check with ``hasattr``. Input: audio."""
TANH: int
"""``TANH_LEVEL * tanh(TANH_DRIVE * x)`` with ``x`` in units of `UNIT`. The same bytes on every interpreter. Input: audio."""
PAN: int
"""A stereo mixer of up to eight inputs with a balance on each: the left side goes to the node's output and the right to the buffer given as ``arg`` when the node is added. Parameters 0 to 7 are a gain per input, and `PAN_PAN` plus ``i`` is input ``i``'s pan."""
WAVEFOLD: int
"""A West Coast wavefolder: three sine folds crossfaded with the input by the fold. Inputs: audio, fold CV."""
LPG: int
"""A vactrol low-pass gate. Inputs: audio, control, strike."""
MODAL: int
"""Eight tuned resonators struck or rung by the input: bells, bars and drum heads, `audiomodal.Bank`'s kernel. Inputs: excitation, strike."""
STRUCK: int
"""A hammer or pluck exciter into a 24-mode struck bar bank, for tine, reed, vibraphone and clavinet voices. `Graph.table` replaces its modes. Inputs: strike, velocity."""
PICKUP: int
"""An electric piano pickup's asymmetric nonlinearity with DC removal. Input: audio."""
TREMOLO: int
"""A mono-to-stereo auto-pan or amplitude tremolo. Built with ``arg`` the right side's buffer, like `PAN`. Inputs: audio, rate CV, depth CV."""
DECAY: int
"""A one-shot exponential envelope, a burst of re-strikes, or the two-stage, soft-attack and rise shapes a drum needs. `Graph.table` sets the strikes. Inputs: trigger, velocity."""
PINK: int
"""Pink noise: Paul Kellet's pinking filter on a white input. Input: white."""
CRUSH: int
"""Rounds the audio to a number of bits. Input: audio."""
ACID: int
"""A diode ladder filter with an accent circuit, as a TB-303 style bass line needs. Inputs: audio, env, accent."""
SPRING: int
"""A two-tank spring reverb over `audiofreeverb`, mixed on both sides. Built with ``arg`` the right side's buffer. Input: audio."""
ADSR2: int
"""An `ENV` contour that starts at `ADSR2_INITIAL` and peaks at `ADSR2_ATTACK_LEVEL`. Input: gate."""
GLISS: int
"""`LAG`'s linear glide stepped onto a semitone grid once a block. Inputs: note, pitch, legato."""
FOLLOW: int
"""An envelope follower, one level a block, peak or RMS. Input: audio."""
WAVETABLE: int
"""A drawbar organ voice from one stored cycle. Inputs: pitch, gate and an extra signal added before the contacts. `Graph.table` holds the cycle; ``arg`` is the most samples it may hold."""
ROTARY: int
"""A rotary speaker, stereo like `PAN`: ``arg`` is the right side's buffer. Input: audio."""

# Groups and the wire

ALWAYS: int
"""A group that always runs, whatever mask `Graph.run` is given. Pass it as a node's ``group``."""
UNIT: int
"""The wire's 1.0 in counts: 16384. A buffer holds 16-bit samples, so audio at 0.5 is a quarter of full scale."""

# ENV, ADSR2 and DECAY

ENV_ATTACK: int
"""`ENV` parameter: the attack time in seconds."""
ENV_DECAY: int
"""`ENV` parameter: the decay time in seconds."""
ENV_SUSTAIN: int
"""`ENV` parameter: the sustain level, 0.0 to 1.0."""
ENV_RELEASE: int
"""`ENV` parameter: the release time in seconds."""
ADSR2_ATTACK: int
"""`ADSR2` parameter: the attack time in seconds."""
ADSR2_DECAY: int
"""`ADSR2` parameter: the decay time in seconds."""
ADSR2_SUSTAIN: int
"""`ADSR2` parameter: the sustain level, 0.0 to 1.0."""
ADSR2_RELEASE: int
"""`ADSR2` parameter: the release time in seconds."""
ADSR2_INITIAL: int
"""`ADSR2` parameter: the level the contour starts from."""
ADSR2_ATTACK_LEVEL: int
"""`ADSR2` parameter: the level the attack peaks at."""
DECAY_DECAY: int
"""`DECAY` parameter: seconds for the envelope to fall by 40 dB."""
DECAY_RESTRIKE: int
"""`DECAY` parameter: with a strike table, seconds to fall by 40 dB between spikes."""
DECAY_ATTACK: int
"""`DECAY` parameter: seconds of linear ramp from 0 up to the velocity before the fall begins."""
DECAY_FAST: int
"""`DECAY` parameter: seconds to fall by 40 dB for the first `DECAY_FAST_SEC` seconds, then `DECAY_DECAY` for the rest."""
DECAY_FAST_SEC: int
"""`DECAY` parameter: how long the fast fall lasts, in seconds."""
DECAY_RISE: int
"""`DECAY` parameter: seconds to fall by 40 dB of a second envelope that is subtracted from the first."""
DECAY_STRIKES: int
"""The most strikes `Graph.table` takes for a `DECAY` node: 16."""

# LFO and VCA

LFO_RATE: int
"""`LFO` parameter: the rate in Hz."""
LFO_AMPLITUDE: int
"""`LFO` parameter: the swing in counts, plus and minus."""
LFO_SHAPE: int
"""`LFO` parameter: one of the ``LFO_*`` shapes. Every shape comes from one phase, so LFOs of different shapes at one rate stay in step."""
LFO_TRIANGLE: int
"""An `LFO_SHAPE` value: a triangle whose bottom is at the start of the cycle."""
LFO_SINE: int
"""An `LFO_SHAPE` value: a sine."""
LFO_SAW: int
"""An `LFO_SHAPE` value: a falling sawtooth."""
LFO_SQUARE: int
"""An `LFO_SHAPE` value: plus the amplitude for the first half of the cycle, minus for the second."""
LFO_RANDOM: int
"""An `LFO_SHAPE` value: a new level, uniform in plus and minus the amplitude, at the start of each cycle, held until the next."""
MIX_OFFSET: int
"""`MIX` parameter: an offset in counts added to the sum."""
VCA_GAIN: int
"""`VCA` parameter: a gain applied after the control input."""
VCA_SELECT: int
"""`VCA` parameter: which control input to use: ``cv_a`` below 0.5, else ``cv_b``."""
VCA_CURVE: int
"""`VCA` parameter: `VCA_LINEAR` or `VCA_EXPONENTIAL`."""
VCA_LINEAR: int
"""A `VCA_CURVE` value: the gain follows the control input."""
VCA_EXPONENTIAL: int
"""A `VCA_CURVE` value: 60 dB over the control input, unity at `UNIT`."""

# LAG, GLISS and SAMPLE_HOLD

LAG_TIME: int
"""`LAG` parameter: the glide time in seconds."""
LAG_MODE: int
"""`LAG` parameter: `LAG_OFF`, `LAG_ON` (the default) or `LAG_AUTO`."""
LAG_SHAPE: int
"""`LAG` parameter: `LAG_LINEAR` (the default) or `LAG_ONE_POLE`."""
LAG_OFF: int
"""A `LAG_MODE` value: no glide."""
LAG_ON: int
"""A `LAG_MODE` value: always glide."""
LAG_AUTO: int
"""A `LAG_MODE` value: glide only when the legato input is above 0.5 at the start of the block."""
LAG_LINEAR: int
"""A `LAG_SHAPE` value: a straight line that takes `LAG_TIME` seconds wherever it starts from."""
LAG_ONE_POLE: int
"""A `LAG_SHAPE` value: a one-pole approach. It ignores the mode."""
GLISS_TIME: int
"""`GLISS` parameter: the glide time in seconds."""
GLISS_MODE: int
"""`GLISS` parameter: `LAG_OFF`, `LAG_ON` or `LAG_AUTO`."""
GLISS_STEP: int
"""`GLISS` parameter: the grid the pitch is stepped onto, where 0.1 is a semitone at one volt per octave."""
SH_LAG: int
"""`SAMPLE_HOLD` parameter: a per-sample one-pole coefficient from 0 to 1 that slews the output toward the held value. 0 does not slew."""
SH_RATE: int
"""`SAMPLE_HOLD` parameter: with the clock input open, how many times a second to refresh."""

# Filters and shapers

TWOPOLE_TYPE: int
"""`TWOPOLE` parameter: one of the ``TWOPOLE_*`` filter types."""
TWOPOLE_CUTOFF_HZ: int
"""`TWOPOLE` parameter: the cutoff in Hz, before the cutoff CV."""
TWOPOLE_DEPTH: int
"""`TWOPOLE` parameter: how far the cutoff CV moves the cutoff; 1 is an octave per 1638.4 counts, 0 ignores the input."""
TWOPOLE_RESONANCE: int
"""`TWOPOLE` parameter: the Sallen-Key's peak or the CMOS filter's resonance, 0.0 to 1.0."""
TWOPOLE_DRIVE: int
"""`TWOPOLE` parameter: a gain applied to the input before the filter."""
TWOPOLE_SK_LOWPASS: int
"""A `TWOPOLE_TYPE` value: a Sallen-Key low-pass whose peak feedback runs through a diode clipper."""
TWOPOLE_SK_HIGHPASS: int
"""A `TWOPOLE_TYPE` value: a Sallen-Key high-pass."""
TWOPOLE_CMOS_LOWPASS: int
"""A `TWOPOLE_TYPE` value: a CMOS state-variable low-pass, with a clipping inverter in every stage."""
TWOPOLE_CMOS_BANDPASS: int
"""A `TWOPOLE_TYPE` value: the CMOS filter as a band-pass."""
TWOPOLE_CMOS_HIGHPASS: int
"""A `TWOPOLE_TYPE` value: the CMOS filter as a high-pass."""
OVERDRIVE_DRIVE: int
"""`OVERDRIVE` parameter: the pre-gain, 0.0 to 1.0, up to 20 dB."""
OVERDRIVE_TONE: int
"""`OVERDRIVE` parameter: the shelf's gain, 0.0 to 1.0, from minus 12 dB through flat at 0.5 to plus 12 dB."""
OVERDRIVE_LEVEL: int
"""`OVERDRIVE` parameter: the linear gain after the clipper; 1 is unity."""
TANH_DRIVE: int
"""`TANH` parameter: a gain before the tanh; 1 is unity."""
TANH_LEVEL: int
"""`TANH` parameter: a gain after the tanh; 1 is unity."""
PAN_PAN: int
"""`PAN` parameter: the index of the first pan; input ``i``'s pan is ``PAN_PAN + i``, from -1 (left) to 1 (right)."""
WAVEFOLD_FOLD: int
"""`WAVEFOLD` parameter: how far the signal is folded, 0.0 to 1.0. A fold CV adds to it."""
WAVEFOLD_SYMMETRY: int
"""`WAVEFOLD` parameter: an offset before the folds, -1.0 to 1.0."""
LPG_MODE: int
"""`LPG` parameter: `LPG_VCF`, `LPG_VCA` or `LPG_BOTH`."""
LPG_DECAY: int
"""`LPG` parameter: seconds for the vactrol to fall by a factor of e."""
LPG_MANUAL: int
"""`LPG` parameter: a manual opening, 0.0 to 1.0, added to the control and strike inputs."""
LPG_LEVEL: int
"""`LPG` parameter: a gain applied last."""
LPG_VCF: int
"""An `LPG_MODE` value: low-pass the input at a cutoff the vactrol moves, 30 Hz closed to 12 kHz open."""
LPG_VCA: int
"""An `LPG_MODE` value: scale the input by the vactrol."""
LPG_BOTH: int
"""An `LPG_MODE` value, and the default: filter and scale, so the highs die before the lows."""
PICKUP_MODE: int
"""`PICKUP` parameter: `PICKUP_ELECTROMAGNETIC` or `PICKUP_ELECTROSTATIC`."""
PICKUP_DRIVE: int
"""`PICKUP` parameter: a gain applied to the input."""
PICKUP_OFFSET: int
"""`PICKUP` parameter: a shift added before the nonlinearity; above 0 it grows second-harmonic bark."""
PICKUP_LEVEL: int
"""`PICKUP` parameter: a gain applied last."""
PICKUP_ELECTROMAGNETIC: int
"""A `PICKUP_MODE` value, and the default: a Rhodes-style coil."""
PICKUP_ELECTROSTATIC: int
"""A `PICKUP_MODE` value: a Wurlitzer-style reed plate."""
CRUSH_BITS: int
"""`CRUSH` parameter: the bit depth, 1 to 15."""
ACID_CUTOFF_HZ: int
"""`ACID` parameter: the cutoff in Hz."""
ACID_RESONANCE: int
"""`ACID` parameter: the resonance, 0.0 to 1.0."""
ACID_ENVMOD: int
"""`ACID` parameter: how far the envelope input moves the cutoff, 0.0 to 1.0."""
PINK_LEVEL: int
"""`PINK` parameter: the output level."""

# Modal, struck and spring

MODAL_FREQUENCY: int
"""`MODAL` parameter: the base frequency in Hz."""
MODAL_DECAY: int
"""`MODAL` parameter: the ring time in seconds."""
MODAL_BRIGHTNESS: int
"""`MODAL` parameter: how much the upper modes sound, 0.0 to 1.0."""
MODAL_INPUT: int
"""`MODAL` parameter: a gain on the excitation before the bank."""
MODAL_MIX: int
"""`MODAL` parameter: from the excitation alone (0.0) to the bank alone (1.0)."""
MODAL_RATIO: int
"""`MODAL` parameter: the index of the first mode ratio; mode ``m`` sits at the frequency times ``MODAL_RATIO + m``, for ``m`` 0 to 7. A ratio of 0 turns its mode off."""
MODAL_MODES: int
"""The number of modes of a `MODAL` node: 8."""
STRUCK_FREQUENCY: int
"""`STRUCK` parameter: the note's frequency in Hz."""
STRUCK_HARDNESS: int
"""`STRUCK` parameter: 0.0 to 1.0; harder shortens the strike's pulse while keeping its area."""
STRUCK_LEVEL: int
"""`STRUCK` parameter: the strike's level."""
STRUCK_SHAPE: int
"""`STRUCK` parameter: `STRUCK_HAMMER` or `STRUCK_PLUCK`."""
STRUCK_HAMMER: int
"""A `STRUCK_SHAPE` value: a half-sine pulse."""
STRUCK_PLUCK: int
"""A `STRUCK_SHAPE` value: a rising ramp."""
STRUCK_MODES: int
"""The most modes `Graph.table` takes for a `STRUCK` node: 24."""
SPRING_MIX: int
"""`SPRING` parameter: dry to wet, 0.0 to 1.0."""
SPRING_DECAY: int
"""`SPRING` parameter: the tail's length, 0.0 to 1.0."""
SPRING_DAMP: int
"""`SPRING` parameter: how fast the highs die, 0.0 to 1.0."""

# Tremolo, follower, wavetable and rotary

TREMOLO_RATE: int
"""`TREMOLO` parameter: the rate in Hz, at least 0.01."""
TREMOLO_DEPTH: int
"""`TREMOLO` parameter: the depth, 0.0 to 1.0. A change glides across the next block."""
TREMOLO_MODE: int
"""`TREMOLO` parameter: `TREMOLO_PAN` or `TREMOLO_AMPLITUDE`."""
TREMOLO_PAN: int
"""A `TREMOLO_MODE` value, and the default: stereo auto-pan."""
TREMOLO_AMPLITUDE: int
"""A `TREMOLO_MODE` value: the same amplitude tremolo on both sides."""
FOLLOW_ATTACK: int
"""`FOLLOW` parameter: the attack time in seconds."""
FOLLOW_RELEASE: int
"""`FOLLOW` parameter: the release time in seconds."""
FOLLOW_MODE: int
"""`FOLLOW` parameter: `FOLLOW_PEAK` or `FOLLOW_RMS`."""
FOLLOW_PEAK: int
"""A `FOLLOW_MODE` value: follow the peak."""
FOLLOW_RMS: int
"""A `FOLLOW_MODE` value: follow the RMS level."""
WAVETABLE_FREQUENCY: int
"""`WAVETABLE` parameter: the frequency in Hz."""
ROTARY_SPEED: int
"""`ROTARY` parameter: `ROTARY_STOP`, `ROTARY_SLOW` or `ROTARY_FAST`."""
ROTARY_DRIVE: int
"""`ROTARY` parameter: the drive, 0 to 10."""
ROTARY_VOLUME: int
"""`ROTARY` parameter: the volume, 0 to 10."""
ROTARY_STOP: int
"""A `ROTARY_SPEED` value: the rotors stopped."""
ROTARY_SLOW: int
"""A `ROTARY_SPEED` value: the slow speed."""
ROTARY_FAST: int
"""A `ROTARY_SPEED` value: the fast speed."""

# The kernels' option numbers, as the modules that wrap them number them

OSC_FREQUENCY: int
"""`OSC` parameter: the frequency in Hz, before the pitch input."""
OSC_WAVEFORM: int
"""`OSC` parameter: 1 for a sawtooth, 2 pulse, 4 triangle, 8 sine; a sum plays those added."""
OSC_PULSE_WIDTH: int
"""`OSC` parameter: the pulse width, 0.05 to 0.95."""
OSC_LEVEL: int
"""`OSC` parameter: the output level."""
OSC_PITCH_OCTAVES: int
"""`OSC` parameter: octaves for a full-scale pitch input."""
OSC_FM_DEPTH: int
"""`OSC` parameter: Hz for a full-scale FM input."""
OSC_PWM_DEPTH: int
"""`OSC` parameter: pulse width for a full-scale PWM input."""
LADDER_CUTOFF_HZ: int
"""`LADDER` parameter: where the filter turns over, in Hz."""
LADDER_RESONANCE: int
"""`LADDER` parameter: 0 to 4.2; at 4 it sustains a tone at the cutoff."""
LADDER_DRIVE: int
"""`LADDER` parameter: the linear gain into the loop; 1 is unity."""
LADDER_POLES: int
"""`LADDER` parameter: the tap, 1 to 4 stages, 6 to 24 dB an octave."""
LADDER_PASSBAND_COMP: int
"""`LADDER` parameter: 0.0 to 1.0, how much of the passband to give back."""
LADDER_OVERSAMPLE: int
"""`LADDER` parameter: 1 or 2."""
LADDER_MIX: int
"""`LADDER` parameter: 0.0 is a wire, 1.0 is the filter."""
LADDER_CUTOFF_OCTAVES: int
"""`LADDER` parameter: octaves for a full-scale cutoff input."""
BIQUAD_MODE: int
"""`BIQUAD` parameter: a mode numbered as `audiobiquad` numbers them."""
BIQUAD_FREQUENCY: int
"""`BIQUAD` parameter: the corner or centre frequency in Hz."""
BIQUAD_Q: int
"""`BIQUAD` parameter: the quality factor, 0.05 to 60."""
BIQUAD_GAIN_DB: int
"""`BIQUAD` parameter: the gain in decibels, for the peaking and shelf modes."""
BIQUAD_MIX: int
"""`BIQUAD` parameter: 0.0 is a wire, 1.0 is the filtered signal alone."""
BIQUAD_RAMP: int
"""`BIQUAD` parameter: milliseconds the filter takes to walk to new coefficients, 0 to 1000; the default is 20. The walk is the power of two frames nearest the request, and 0 is one frame. Set it low when a value moves every block."""
DELAY_MS: int
"""`FBDELAY` parameter: the delay in milliseconds."""
DELAY_FEEDBACK: int
"""`FBDELAY` parameter: the feedback."""
DELAY_MIX: int
"""`FBDELAY` parameter: the wet mix."""
DELAY_DAMPING_HZ: int
"""`FBDELAY` parameter: the damping filter's corner in Hz."""
DELAY_WOW_HZ: int
"""`FBDELAY` parameter: the wow modulation's rate in Hz."""
DELAY_WOW_DEPTH_MS: int
"""`FBDELAY` parameter: the wow modulation's depth in milliseconds."""
PHASER_FREQUENCY: int
"""`PHASER` parameter: the frequency in Hz at which one stage's phase passes minus 90 degrees."""
PHASER_FEEDBACK: int
"""`PHASER` parameter: the feedback around the chain, -0.99 to 0.99."""
PHASER_MIX: int
"""`PHASER` parameter: dry to wet, 0.0 to 1.0."""

class Graph:
    """A patch of nodes that renders a block at a time.

    Every argument is a keyword.

    A `Graph` is also an audio source: hand it to an output, a mixer or the
    pump and each pull renders one block, and it never ends.

    `set`, `reset` and `enable` do not touch the running graph. Each goes into
    the queue, and the renderer applies everything waiting at the top of the
    next block it renders, so a change never lands halfway through a block.
    Between `hold` and `commit` nothing is released, so a batch lands on one
    block together. When the queue fills, the control side waits for the pump
    to finish its block and applies what is waiting itself, so nothing is
    dropped; a held batch that overflowed lands across two blocks.

    While the pump is pulling a graph, the calls that change its shape
    (`buffer`, `add`, `table` and `write`) and a `run` of your own raise
    `RuntimeError`. Build the graph before you hand it over. `read` and
    `peak` still work, but can catch a buffer the pump is rendering into.

    Args:
        sample_rate: The patch's rate in Hz, 1 or more. The default is 48000.
        frames: Frames in a block, 1 to 256. The default is 128.
        channel_count: Channels of the output, 1 or 2.
        buffers: How many block buffers the graph owns, 1 to 4096. The
            default is 64.
        nodes: How many nodes the table holds, 1 to 4096. The default is 64.
        queue: How many changes the control queue holds, 1 to 65536. The
            default, 0, is the larger of 256 and four per node.

    Raises:
        ValueError: An argument is out of range.
    """

    def __init__(
        self,
        *,
        sample_rate: int = 48000,
        frames: int = 128,
        channel_count: int = 2,
        buffers: int = 64,
        nodes: int = 64,
        queue: int = 0,
    ) -> None: ...
    sample_rate: int
    """Frames per second, as given to the constructor."""
    @property
    def bits_per_sample(self) -> int:
        """Size of one sample in bits: always 16."""
    @property
    def channel_count(self) -> int:
        """Number of channels: 1 or 2."""
    def buffer(self) -> int:
        """Hand out a fresh block buffer.

        Returns:
            The buffer's id, to pass to `add`, `read`, `write` and `peak`.

        Raises:
            ValueError: No buffers are left.
            RuntimeError: The pump is rendering this graph.
        """
    def add(
        self,
        kind: int,
        group: int,
        inputs: Sequence[int | None],
        output: int | None,
        arg: float = 0.0,
        /,
    ) -> int:
        """Append a node.

        Nodes run in the order you add them, so add a node after the nodes it
        reads.

        Args:
            kind: A node kind, such as `OSC`.
            group: 0 to 31, or `ALWAYS`. `run` skips a node whose group bit is
                not set in its ``enable`` mask, and a node skipped after it ran
                zeroes its output once.
            inputs: Buffer ids the node reads, up to eight. None is an open
                input.
            output: The buffer id the node writes. Every kind but `OUT` needs
                one.
            arg: A setting fixed at construction. What it means depends on the
                kind: the line's length for `FBDELAY`, the stages for
                `PHASER`, the right side's buffer for `PAN`, `TREMOLO`,
                `SPRING` and `ROTARY`.

        Returns:
            The node's index.

        Raises:
            ValueError: The kind is unknown, the group is out of range, there
                are too many inputs, a buffer id is out of range, the node has
                no output, or the graph is full.
            RuntimeError: The pump is rendering this graph.
        """
    def set(self, node: int, index: int, value: float, /) -> None:
        """Store a parameter. The change is queued and lands at the top of the
        next block. A parameter that has not changed costs nothing per block.

        Args:
            node: The node's index.
            index: The parameter, such as `OSC_WAVEFORM`, or a number for a
                kind that numbers its parameters from 0.
            value: The new value.

        Raises:
            ValueError: The node or parameter does not exist.
        """
    def get(self, node: int, index: int, /) -> float:
        """The value `set` last stored, whether or not a block has heard it.

        Raises:
            ValueError: The node or parameter does not exist.
        """
    def table(self, node: int, table: object, /) -> None:
        """Give a node a table.

        What the table is depends on the node. For `FBDELAY` it is the wow
        shape, an ``array('h')`` the graph keeps a reference to and does not
        copy. For `STRUCK` it is up to `STRUCK_MODES` ``(ratio, decay, gain)``
        triples, or a flat float array of three floats a mode. For `DECAY` it
        is up to `DECAY_STRIKES` ``(delay in seconds, level)`` pairs, or a flat
        float array of two floats a strike. For `WAVETABLE` it is one cycle,
        an ``array('h')`` of 2 samples up to the node's ``arg``. A copied table
        may change while the pump plays; a borrowed one may not.

        For `STRUCK`, set `STRUCK_FREQUENCY` first and then call this: it
        retunes the bank at the frequency you last set, so the block that
        plays the note only copies the result.

        Raises:
            TypeError: The table is not of the type the node takes.
            ValueError: The node takes no table, or the table is too long.
            RuntimeError: The pump is rendering this graph (not for tables the
                node copies).
        """
    def reset(self, node: int, /) -> None:
        """Clear a node's running state. Queued, like `set`.

        Raises:
            ValueError: The node does not exist.
        """
    def carry(self, node: int, src_graph: Graph, src_node: int, /) -> None:
        """Take over a node's running state from a node of another graph, or
        of this one: its voice, envelope, oscillator phase, filter memory and
        delay line continue where ``src_node`` left them. Build the changed
        patch beside the running one, carry the nodes both have, then
        `swap` it in. Do it between two blocks.

        Args:
            node: The node of this graph that receives the state.
            src_graph: The graph the state comes from.
            src_node: The node in ``src_graph``.

        Raises:
            TypeError: ``src_graph`` is not a `Graph`.
            ValueError: The nodes are of different kinds, were built with
                different ``arg`` values, hold state of different sizes, or
                belong to graphs at different sample rates. A node it refuses
                keeps the fresh state it was built with.
            RuntimeError: The pump is rendering this graph; park it first.
        """
    def swap(self, other: Graph, /) -> None:
        """Trade patches with ``other`` on a block boundary: nodes, buffers,
        wiring and queue. This graph keeps its identity and its frame count,
        so what holds it (the pump, an output, a mixer) need not know, and
        renders ``other``'s patch from the next block on. ``other`` is left
        holding the old patch, ready to drop. Node ids are then the ones you
        got from building ``other``.

        Raises:
            TypeError: ``other`` is not a `Graph`.
            ValueError: The graphs differ in sample rate, block size or
                channel count.
        """
    def enable(self, mask: int, /) -> None:
        """Set the groups a pull renders, as a bit mask. Queued like `set`, so
        a note's gate and its group's enable land in the same block."""
    def hold(self) -> None:
        """Stop releasing queued changes until `commit`, so a batch (a patch
        change, a chord) lands on one block together."""
    def commit(self) -> None:
        """Release what was queued since `hold`."""
    def sounding(self) -> tuple[int, int, int, int]:
        """What the renderer last published.

        Returns:
            ``(sounding, frame, applied, pushed)``: the mask of groups still
            sounding after the last block, the frames rendered so far, and the
            number of changes applied and queued. A change counted in
            ``pushed`` and not yet in ``applied`` has not been heard. Read
            ``sounding`` and ``frame`` together and you know which block the
            mask belongs to.
        """
    def stats(self) -> dict[str, int]:
        """The queue's counts.

        Returns:
            A dict with ``capacity``, ``pushed``, ``applied``, ``overflows``
            (times the queue filled), ``splits`` (held batches that landed
            across two blocks), ``high_water`` (the most changes waiting at
            once) and ``frame``. Size the queue for your biggest batch and
            ``overflows`` and ``splits`` stay 0.
        """
    def run(
        self,
        out: bytearray | memoryview | None = None,
        block: int = 0,
        enable: int = 0xFFFFFFFF,
        /,
    ) -> int:
        """Render one block, draining the queue first.

        Runs every node whose group bit is set in ``enable`` and every
        `ALWAYS` node.

        Args:
            out: Where the `OUT` node writes: interleaved signed 16-bit
                samples, ``frames * channel_count`` of them a block. None
                renders without keeping the output.
            block: Which block of ``out`` to fill, counting from 0, so one
                buffer holds several blocks.
            enable: The groups to run, a bit mask. The default runs all.

        Returns:
            The mask of groups whose envelopes are still sounding. When a
            voice's amplitude envelope goes idle, the mask tells you to park
            its group.

        Raises:
            ValueError: ``out`` is too small for that block.
            RuntimeError: The pump is rendering this graph.
        """
    def peak(self, buffer: int, /) -> int:
        """The peak of a buffer's block, as a count.

        Raises:
            ValueError: The buffer id does not exist.
        """
    def read(self, buffer: int, dst: bytearray | memoryview, /) -> None:
        """Copy a buffer's block out into ``dst``, which must hold exactly one
        block of ``frames`` signed 16-bit samples.

        Raises:
            ValueError: The buffer id does not exist, or ``dst`` is the wrong
                size.
        """
    def write(self, buffer: int, src: bytes | bytearray | memoryview, /) -> None:
        """Fill a buffer's block from ``src``, exactly one block of ``frames``
        signed 16-bit samples, so Python can feed a buffer from something the
        graph has no node for.

        Raises:
            ValueError: The buffer id does not exist, or ``src`` is the wrong
                size.
            RuntimeError: The pump is rendering this graph.
        """
    def deinit(self) -> None:
        """Free the graph. A graph released while the pump is pulling it stops
        cleanly."""
    def __enter__(self) -> Self: ...
    def __exit__(self, *args: object) -> None: ...
