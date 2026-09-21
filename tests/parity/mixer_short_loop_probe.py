"""A looping sample too short to fill one packed word must not hang.

    mixer_short_loop_probe.py

Runs unchanged on CPython, on desktop MicroPython and on a board. **Run it
under a timeout** -- `timeout 60 ... mixer_short_loop_probe.py` -- because the
defect it covers is a hang, and an unbounded run that hangs says nothing at
all, where a killed one says everything.

audiodsp#85. The mixer consumes whole 32-bit words: `MixerVoice` divides a
fetched buffer's byte length by `sizeof(uint32_t)`, so a two-byte sample
measures zero words, every pass of the mix-down takes zero of them, and a
looping voice never reaches the exit that stops a finished one. On the native
builds that is an unbounded spin.

Two things stop it, and this probe exercises both. `play(loop=True)` refuses a
source whose first fetch is under a word with `ValueError`, which is the route
a program takes and the one that gives the programmer something to act on.
`voice.loop = True` set afterwards is not refused -- at that moment an empty
buffer is also what the end of any sample looks like -- so the mix-down carries
a backstop instead: a second consecutive fetch that yields no word stops the
voice, as the CPython twin has done since audiodsp#24.

**CircuitPython 10.3.0 hangs on case 1** and is not in the comparison for that
reason; `audiomixer` is a stock CP module, this port fixes its own two targets,
and the deviation is written up in docs/upstream-diff.md. So this file is not
in `verify_dsp.py`'s PROBES -- adding it there would hang the three-way on the
oracle rather than report anything.
"""

import sys
from array import array

import audiocore
import audiomixer

SAMPLE_RATE = 48000
BUFFER_SIZE = 512

failures = 0


def report(name, ok, detail=""):
    global failures
    if not ok:
        failures += 1
    print("%-34s %s %s" % (name, "ok" if ok else "FAIL", detail))


def mixer(channel_count):
    return audiomixer.Mixer(voice_count=1, sample_rate=SAMPLE_RATE,
                            channel_count=channel_count, bits_per_sample=16,
                            samples_signed=True, buffer_size=BUFFER_SIZE)


def sample(frames, channel_count):
    return audiocore.RawSample(array("h", [0] * (frames * channel_count)),
                               sample_rate=SAMPLE_RATE,
                               channel_count=channel_count)


def rendered(mix):
    _result, buffer = audiocore.get_buffer(mix)
    return len(bytes(buffer))


# 1. The defect itself: one mono frame is two bytes, half a word.
mix = mixer(1)
raised = None
try:
    mix.voice[0].play(sample(1, 1), loop=True)
except ValueError as error:
    raised = str(error)
report("mono 1 frame, loop=True", raised is not None,
       "raised %r" % raised if raised else "NO ValueError -- get_buffer would "
       "have hung")

# 2. ...and the voice is left stopped rather than half-started, so a caller
#    that catches the error does not then render from a refused sample.
report("refused play leaves it stopped", not mix.voice[0].playing)

# 3. The same sample not looping is untouched: it was never the hang, it
#    renders as silence, and refusing it would break working code. This is
#    also where the block size comes from, rather than being assumed: a Mixer
#    is double-buffered, so one get_buffer is half of buffer_size.
mix = mixer(1)
mix.voice[0].play(sample(1, 1))
BLOCK = rendered(mix)
report("mono 1 frame, loop=False", BLOCK == BUFFER_SIZE // 2,
       "%d bytes" % BLOCK)

# 4. Two mono frames is one whole word -- the documented workaround, and the
#    boundary the rule is stated at. It must NOT be refused.
mix = mixer(1)
mix.voice[0].play(sample(2, 1), loop=True)
report("mono 2 frames, loop=True", rendered(mix) == BLOCK)

# 5. One stereo frame is also one whole word. The bound is bytes, not frames,
#    and a rule stated in frames would wrongly refuse this.
mix = mixer(2)
mix.voice[0].play(sample(1, 2), loop=True)
report("stereo 1 frame, loop=True", rendered(mix) == BLOCK)

# 6. The backstop, reached the way play() cannot see: loop switched on after
#    the fact. It must return, and it must stop the voice rather than spin.
mix = mixer(1)
mix.voice[0].play(sample(1, 1))
mix.voice[0].loop = True
size = rendered(mix)
report("loop=True set after play", size == BLOCK and
       not mix.voice[0].playing, "%d bytes, playing=%s"
       % (size, mix.voice[0].playing))

print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
