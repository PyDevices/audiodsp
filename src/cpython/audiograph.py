"""The graph runner: a whole patch rendered by one C call per block.

audiodsp's own, not a CircuitPython module. The runner is
``src/shared/audiodsp_graph.c``, the same C the MicroPython and CircuitPython
builds compile, so a graph built here renders the samples a board renders.

    import audiograph as ag

    g = ag.Graph(sample_rate=48000, frames=128, channel_count=2)
    gate, env, saw, out = (g.buffer() for _ in range(4))
    k = g.add(ag.CONST, 0, (), gate)
    g.add(ag.ENV, 0, (gate,), env)
    o = g.add(ag.OSC, 0, (None, None, None, None), saw)
    g.set(o, ag.OSC_WAVEFORM, 1)
    g.add(ag.VCA, 0, (saw, env, None), out)
    g.add(ag.OUT, ag.ALWAYS, (out, out), None)

    pcm = bytearray(128 * 2 * 2)
    g.set(k, 0, ag.UNIT)                 # key down
    sounding = g.run(pcm, 0)             # one block into pcm

``docs/how-it-works/audiograph.md`` has the nodes, the wire, the groups and the limits.
"""

import _audiodsp

#: The audiodsp this was built from (src/cp_compat/audiodsp_build.h).
__version__ = _audiodsp.__version__
__revision__ = _audiodsp.__revision__

#: The runner itself, the C type as it is: nothing in Python sits between a
#: caller and run().
Graph = _audiodsp.Graph

# Read by name from the C enums, so the numbers cannot drift from the ones
# the board builds export.
_K = _audiodsp.GRAPH_CONSTANTS

# node kinds
CONST = _K["CONST"]
OSC = _K["OSC"]
LADDER = _K["LADDER"]
BIQUAD = _K["BIQUAD"]
FBDELAY = _K["FBDELAY"]
ENV = _K["ENV"]
LFO = _K["LFO"]
NOISE = _K["NOISE"]
MIX = _K["MIX"]
VCA = _K["VCA"]
OUT = _K["OUT"]
PHASER = _K["PHASER"]
LAG = _K["LAG"]
SAMPLE_HOLD = _K["SAMPLE_HOLD"]
ALWAYS = _K["ALWAYS"]
UNIT = _K["UNIT"]

# parameter indices of the runner's own nodes
ENV_ATTACK = _K["ENV_ATTACK"]
ENV_DECAY = _K["ENV_DECAY"]
ENV_SUSTAIN = _K["ENV_SUSTAIN"]
ENV_RELEASE = _K["ENV_RELEASE"]
LFO_RATE = _K["LFO_RATE"]
LFO_AMPLITUDE = _K["LFO_AMPLITUDE"]
MIX_OFFSET = _K["MIX_OFFSET"]
VCA_GAIN = _K["VCA_GAIN"]
VCA_SELECT = _K["VCA_SELECT"]
LFO_SHAPE = _K["LFO_SHAPE"]
LFO_TRIANGLE = _K["LFO_TRIANGLE"]
LFO_SINE = _K["LFO_SINE"]
LFO_SAW = _K["LFO_SAW"]
LFO_SQUARE = _K["LFO_SQUARE"]
LFO_RANDOM = _K["LFO_RANDOM"]
LAG_TIME = _K["LAG_TIME"]
LAG_MODE = _K["LAG_MODE"]
LAG_SHAPE = _K["LAG_SHAPE"]
LAG_OFF = _K["LAG_OFF"]
LAG_ON = _K["LAG_ON"]
LAG_AUTO = _K["LAG_AUTO"]
LAG_LINEAR = _K["LAG_LINEAR"]
LAG_ONE_POLE = _K["LAG_ONE_POLE"]
SH_LAG = _K["SH_LAG"]
SH_RATE = _K["SH_RATE"]

# the kernels' option numbers, so a caller needs no other module
OSC_FREQUENCY = _K["OSC_FREQUENCY"]
OSC_WAVEFORM = _K["OSC_WAVEFORM"]
OSC_PULSE_WIDTH = _K["OSC_PULSE_WIDTH"]
OSC_LEVEL = _K["OSC_LEVEL"]
OSC_PITCH_OCTAVES = _K["OSC_PITCH_OCTAVES"]
OSC_FM_DEPTH = _K["OSC_FM_DEPTH"]
OSC_PWM_DEPTH = _K["OSC_PWM_DEPTH"]
LADDER_CUTOFF_HZ = _K["LADDER_CUTOFF_HZ"]
LADDER_RESONANCE = _K["LADDER_RESONANCE"]
LADDER_DRIVE = _K["LADDER_DRIVE"]
LADDER_POLES = _K["LADDER_POLES"]
LADDER_PASSBAND_COMP = _K["LADDER_PASSBAND_COMP"]
LADDER_OVERSAMPLE = _K["LADDER_OVERSAMPLE"]
LADDER_MIX = _K["LADDER_MIX"]
LADDER_CUTOFF_OCTAVES = _K["LADDER_CUTOFF_OCTAVES"]
BIQUAD_MODE = _K["BIQUAD_MODE"]
BIQUAD_FREQUENCY = _K["BIQUAD_FREQUENCY"]
BIQUAD_Q = _K["BIQUAD_Q"]
BIQUAD_GAIN_DB = _K["BIQUAD_GAIN_DB"]
BIQUAD_MIX = _K["BIQUAD_MIX"]
DELAY_MS = _K["DELAY_MS"]
DELAY_FEEDBACK = _K["DELAY_FEEDBACK"]
DELAY_MIX = _K["DELAY_MIX"]
DELAY_DAMPING_HZ = _K["DELAY_DAMPING_HZ"]
DELAY_WOW_HZ = _K["DELAY_WOW_HZ"]
DELAY_WOW_DEPTH_MS = _K["DELAY_WOW_DEPTH_MS"]
PHASER_FREQUENCY = _K["PHASER_FREQUENCY"]
PHASER_FEEDBACK = _K["PHASER_FEEDBACK"]
PHASER_MIX = _K["PHASER_MIX"]

del _K
