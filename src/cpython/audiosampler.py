"""Play recorded notes: N voices, each reading a buffer at its own pitch.

`audiomixer.Mixer` plays sources at their own rate and `synthio` plays
single-cycle waveforms. Neither can take a recorded note, play it a fourth
up, loop a region of it while the key is held and let the rest of it out
when the key lifts. `Sampler` does that, which is what a SoundFont
instrument and every drum machine need:

    sampler = audiosampler.Sampler(voice_count=8, sample_rate=48000)
    audio_out.play(sampler)
    sampler.voice[0].play(piano_c4, rate=44100, pitch=2 ** (5 / 12),
                          loop_start=11000, loop_end=19000,
                          loop_mode=audiosampler.LOOP_UNTIL_RELEASE,
                          attack=0.005, release=0.3)
    ...
    sampler.voice[0].release()

A buffer is mono signed 16-bit: an `array('h')`, a `memoryview` or anything
else with a buffer. `rate` is the buffer's own sample rate (the sampler's
when left out), so the step per output frame is `pitch * rate / sample_rate`
and a 44.1 kHz recording plays at the right speed through a 48 kHz output
without being told anything else.

The three loop modes are SoundFont's `sampleModes`, under their numbers:
`LOOP_NONE` (0) plays to the end and stops, `LOOP_FOREVER` (1) loops
`[loop_start, loop_end)` until `release()` or `stop()`, and
`LOOP_UNTIL_RELEASE` (3) loops while the key is held, then plays on past the
loop to the end of the buffer.

The envelope is a per-voice amplitude DAHDSR, each stage in seconds and
`sustain` a linear level, run per sample in C. `delay` postpones the start
of the sound itself rather than holding a playing sample at zero, so a drum
machine can use it for a flam without a second voice. `level`, `pan` and
`pitch` are live: a change lands at the next block, the first two as a ramp
across it so a fader move is a slope rather than a click.

Every voice is mixed in one C call per block, in float, and quantised once.
Interpolation is a 4-point Hermite, so a loop at pitch 1.0 is a wire.
"""

from audiocore import GET_BUFFER_MORE_DATA, _AudioSample
import _audiodsp


#: The audiodsp this was built from, the same pair the native builds put
#: on this module (src/cp_compat/audiodsp_build.h).
__version__ = _audiodsp.__version__
__revision__ = _audiodsp.__revision__

FRAMES = _audiodsp.SAMPLER_FRAMES
MAX_FRAMES = _audiodsp.SAMPLER_MAX_FRAMES
MAX_VOICES = _audiodsp.SAMPLER_MAX_VOICES
LOOP_NONE = _audiodsp.SAMPLER_LOOP_NONE
LOOP_FOREVER = _audiodsp.SAMPLER_LOOP_FOREVER
LOOP_UNTIL_RELEASE = _audiodsp.SAMPLER_LOOP_UNTIL_RELEASE


class SamplerVoice:
    """One of a `Sampler`'s voices. Made by the sampler, never directly."""

    def __init__(self, sampler, index):
        self._sampler = sampler
        self._index = index
        self._level = 1.0
        self._pan = 0.0
        self._pitch = 1.0

    def play(self, buffer, *, rate=None, pitch=1.0, loop_start=0,
             loop_end=None, loop_mode=LOOP_NONE, level=1.0, pan=0.0,
             delay=0.0, attack=0.0, hold=0.0, decay=0.0, sustain=1.0,
             release=0.0):
        """Start `buffer` (mono signed 16-bit) on this voice, replacing
        whatever it was playing. `loop_end` left out means the end of the
        buffer. Raises ValueError for a loop that does not fit the buffer or
        a `loop_mode` that is not 0, 1 or 3."""
        self._sampler._check()
        self._sampler._state.play(
            self._index, buffer,
            rate=0.0 if rate is None else float(rate), pitch=float(pitch),
            loop_start=int(loop_start),
            loop_end=0 if loop_end is None else int(loop_end),
            loop_mode=int(loop_mode), level=float(level), pan=float(pan),
            delay=float(delay), attack=float(attack), hold=float(hold),
            decay=float(decay), sustain=float(sustain),
            release=float(release))
        self._level = float(level)
        self._pan = float(pan)
        self._pitch = float(pitch)

    def release(self):
        """Let the key go: enter the release stage. In `LOOP_UNTIL_RELEASE`
        the voice also stops looping and plays on to the end."""
        self._sampler._check()
        self._sampler._state.release(self._index)

    def stop(self):
        """Stop at once, no fade."""
        self._sampler._check()
        self._sampler._state.stop(self._index)

    @property
    def playing(self):
        return self._sampler._state.playing(self._index)

    @property
    def level(self):
        return self._level

    @level.setter
    def level(self, value):
        self._sampler._check()
        self._level = float(value)
        self._sampler._state.set_level(self._index, self._level)

    @property
    def pan(self):
        return self._pan

    @pan.setter
    def pan(self, value):
        self._sampler._check()
        self._pan = float(value)
        self._sampler._state.set_pan(self._index, self._pan)

    @property
    def pitch(self):
        return self._pitch

    @pitch.setter
    def pitch(self, value):
        self._sampler._check()
        self._pitch = float(value)
        self._sampler._state.set_pitch(self._index, self._pitch)


class Sampler(_AudioSample):
    def __init__(self, *, voice_count=8, channel_count=2, sample_rate=48000,
                 buffer_size=None, bits_per_sample=16, samples_signed=True):
        voice_count = int(voice_count)
        if not 1 <= voice_count <= MAX_VOICES:
            raise ValueError("voice_count must be 1 to %d" % (MAX_VOICES,))
        channel_count = int(channel_count)
        if channel_count not in (1, 2):
            raise ValueError("channel_count must be 1 or 2")
        if sample_rate < 1:
            raise ValueError("sample_rate must be at least 1")
        # One output format, the one every other audiodsp node of ours
        # renders. The two arguments are here so a Sampler can stand where a
        # Mixer stood, and they say so if asked for anything else.
        if int(bits_per_sample) != 16 or not samples_signed:
            raise ValueError("Sampler renders signed 16-bit only")
        width = 2 * channel_count
        if buffer_size is None:
            frames = FRAMES
        else:
            frames = int(buffer_size) // width
            if not 1 <= frames <= MAX_FRAMES:
                raise ValueError("buffer_size must be %d to %d bytes"
                                 % (width, MAX_FRAMES * width))
        self.sample_rate = int(sample_rate)
        self.bits_per_sample = 16
        self.channel_count = channel_count
        self.samples_signed = True
        self.single_buffer = False
        self.buffer_size = frames * width
        self.max_buffer_length = frames * width
        self._deinited = False
        self._state = _audiodsp.SamplerState(
            sample_rate=self.sample_rate, channel_count=channel_count,
            frames=frames, voice_count=voice_count)
        self.voice = tuple(SamplerVoice(self, index)
                           for index in range(voice_count))

    @property
    def voice_count(self):
        return len(self.voice)

    @property
    def playing(self):
        """True while any voice sounds."""
        return self._state.sounding() > 0

    def stop(self):
        """Stop every voice at once."""
        self._check()
        for voice in self.voice:
            voice.stop()

    def _release(self):
        for index in range(len(self.voice)):
            self._state.stop(index)

    def _reset_buffer(self, single_channel_output=False, audio_channel=0):
        # A host reset keeps every voice as it is, as audiomixer's does: a
        # sampler has no source to rewind, and a note that was sounding when
        # the output restarted should still be sounding afterwards.
        self._check()

    def _get_buffer(self, single_channel_output=False, audio_channel=0):
        self._check()
        return GET_BUFFER_MORE_DATA, self._publish(self._state.render())


__all__ = ("Sampler", "SamplerVoice", "LOOP_NONE", "LOOP_FOREVER",
           "LOOP_UNTIL_RELEASE")
