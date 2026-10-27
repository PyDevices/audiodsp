# Phase 7 acceptance test: run todbot's unmodified `synthtools` engine
# (vendored at tests/vendor/synthtools, see SYNTHTOOLS_COMMIT.txt) against
# the real ported synthio/audiomixer/audiofilters/audiodelays modules and
# render deterministic PCM. Runs unchanged on bin/micropython and
# bin/circuitpython; the runner diffs the printed output byte-for-byte.
#
# This is a headless stand-in for examples/synth_setup.py: same
# Synthesizer -> Mixer -> voice wiring, minus the board-specific
# audiobusio.I2SOut/analogio/keypad. No time.sleep -- note timing is
# driven by a fixed number of get_buffer() pulls, same convention as the
# tier 0-4 parity scripts.

import sys

_filename = __file__.replace("\\", "/")
_here = _filename.rsplit("/", 1)[0] if "/" in _filename else "."
sys.path.insert(0, _here + "/../vendor")

# waves.py's random_phase_wave() reseeds an oscillator's start phase on
# every note-on via random.randint() -- correct and desirable behavior in
# synthtools itself (see tests/vendor/synthtools/waves.py), but MicroPython
# and CircuitPython ship different random.randint() algorithms, so the same
# script would render different (both valid) PCM on each interpreter and
# defeat a byte-exact oracle diff. Neither interpreter's built-in `random`
# module allows monkeypatching an attribute onto it (both raise
# AttributeError), so a whole substitute module is pre-seeded into
# sys.modules before synthtools imports it: a fixed-sequence LCG, identical
# on both interpreters. This is a property of THIS test harness only, not a
# port deviation -- see docs/how-it-works/upstream-diff.md.
class _DeterministicRandom:
    def __init__(self, seed):
        self._state = seed

    def randint(self, a, b):
        self._state = (1103515245 * self._state + 12345) & 0x7FFFFFFF
        return a + self._state % (b - a + 1)


sys.modules["random"] = _DeterministicRandom(1)

import audiocore
import audiofilters
import audiomixer
import synthio

from synthtools import BasslineSynth, Patch, SubtractiveSynth
from synthtools.audio_fx import EffectsChain, set_drive, sync_delay, tracking_filter

SAMPLE_RATE = 22050
CHANNEL_COUNT = 1
BUFFER_SIZE = 1024


def checksum(data):
    """Order-independent sum, kept so the older records stay comparable.

    On its own this was the ENTIRE channel through which ~48 KB of rendered
    PCM reached the gate, and it is invariant under any permutation of the
    bytes: a render with every mixer block rotated by one frame, and a render
    with every block TIME-REVERSED, both left this gate byte-identically
    green. See issue #19.
    """
    return sum(data) % 1000000007


def order_checksum(data):
    """FNV-1a over the same bytes - order- and magnitude-sensitive.

    This is what actually pins the PCM. audiocomponents' instrument probes use
    the same function for the same reason, and the effects and biquad probes
    gained it for the same reason again.
    """
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def pull(source, n):
    out = bytearray()
    for _ in range(n):
        result, buf = audiocore.get_buffer(source)
        if buf is not None and len(buf):
            out += bytes(buf)
    return out


mixer = audiomixer.Mixer(
    sample_rate=SAMPLE_RATE,
    channel_count=CHANNEL_COUNT,
    buffer_size=BUFFER_SIZE,
    voice_count=2,
)
synthesizer = synthio.Synthesizer(sample_rate=SAMPLE_RATE, channel_count=CHANNEL_COUNT)
audiocore.reset_buffer(mixer)

# --- SubtractiveSynth: two-oscillator patch with filter, filter envelope,
# vibrato and an LFO-driven cutoff sweep -- exercises the block graph end
# to end (Math SUM/PRODUCT/LERP/CONSTRAINED_LERP/MID, LFO retrigger,
# in-place envelope buffer rewrites).
# fmt: off
patch1 = Patch(name="fat bass", wave="ASAW", detune=1.004,
               filt_type="LPF", filt_f=800, filt_q=1.4,
               amp_env=[0.01, 0.1, 0.8, 0.4],
               vib_rate=5.5, vib_depth=0.006, vib_delay=0.05,
               fenv_amount=3000, fenv_attack=0.02, fenv_release=0.30, fenv_curve=2,
               filt_lfo_rate=0.4, filt_lfo_amount=300,
               penv_amount=0.02, penv_time=0.05,
               filt_vel=400, fenv_vel=0.5)
# fmt: on
lead = SubtractiveSynth(synthesizer, patch1)

# an EffectsChain: a Distortion effect (tier-4 module). tracking_filter
# needs a mono synth with a filter (see BasslineSynth below, poly synths
# have no `.filter` at all -- that's the ValueError this would otherwise
# raise, confirmed against the docstring, not a bug).
fx = EffectsChain(lead)
drive = fx.add(
    audiofilters.Distortion(
        mode=audiofilters.DistortionMode.OVERDRIVE,
        mix=0.4,
        sample_rate=SAMPLE_RATE,
        channel_count=CHANNEL_COUNT,
        buffer_size=BUFFER_SIZE,
    )
)
set_drive(drive, 0.3)

mixer.voice[0].play(fx.output)
mixer.voice[0].level = 0.5

out = bytearray()
notes1 = (36, 43, 39, 48)
for i, n in enumerate(notes1):
    lead.note_on(n, velocity=100 + i * 5)
    out += pull(mixer, 6)
    lead.note_off(n)
    out += pull(mixer, 4)
    if i == 1:
        # live parameter sweep mid-run: one shared-block write should
        # reach the already-sounding voice
        lead.filt_f = 1600
        lead.wave = "ASQU"

print("lead nbytes", len(out), "checksum", checksum(out),
      "order", order_checksum(out))

lead.pitch_bend(0.02)
lead.note_on(45, velocity=110)
out2 = pull(mixer, 8)
lead.pitch_bend(0.0)
out2 += pull(mixer, 4)
lead.note_off(45)
out2 += pull(mixer, 6)
print("bend nbytes", len(out2), "checksum", checksum(out2),
      "order", order_checksum(out2))

patch_json = patch1.to_json()
lead2 = SubtractiveSynth(synthesizer, Patch.from_json(patch_json))
print("json roundtrip filt_f", lead2.filt_f, "wave", lead2.wave, "detune", lead2.detune)

# --- BasslineSynth: monophonic, decay-only filter envelope, per-step
# glide + accent, plus a synced Echo -- exercises tier-2 mono glide path
# and audiodelays.
import audiodelays  # noqa: E402  (mirrors BasslineSynth's own lazy-style import)

bass_patch = Patch(
    name="acid",
    wave="ASQU",
    filt_type="LPF",
    filt_f=300,
    filt_q=6.0,
    amp_env=[0.002, 0.05, 0.0, 0.05],
)
bass = BasslineSynth(synthesizer, bass_patch)
bass.glide_time = 0.03

bass_fx = EffectsChain(bass)
bass_fx.add(tracking_filter(bass, stages=1, buffer_size=BUFFER_SIZE))

echo = audiodelays.Echo(
    delay_ms=250.0,
    decay=0.35,
    mix=0.3,
    sample_rate=SAMPLE_RATE,
    channel_count=CHANNEL_COUNT,
    buffer_size=BUFFER_SIZE,
)
sync_delay(echo, bpm=130, steps=2)
echo.play(bass_fx.output)
mixer.voice[1].play(echo)
mixer.voice[1].level = 0.4

out3 = bytearray()
steps = ((33, False, False), (33, True, False), (40, False, True), (33, False, False))
for note, slide, accent in steps:
    velocity = 127 if accent else 100
    bass.note_on(note, velocity=velocity, glide=(bass.glide_time if slide else 0.0))
    out3 += pull(mixer, 5)
    bass.note_off(note)
    out3 += pull(mixer, 3)

print("bass nbytes", len(out3), "checksum", checksum(out3),
      "order", order_checksum(out3))
print("echo delay_ms", echo.delay_ms)

lead.all_notes_off()
bass.all_notes_off()
mixer.voice[0].stop()
mixer.voice[1].stop()
out4 = pull(mixer, 4)
print("tail nbytes", len(out4), "checksum", checksum(out4),
      "order", order_checksum(out4))

print("mixer playing", mixer.playing)
print("done")
