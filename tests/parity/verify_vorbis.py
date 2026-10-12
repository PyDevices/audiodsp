"""audiovorbis against ffmpeg: the same files, the same samples within Tremor's error.

    verify_vorbis.py [--micropython PATH] [--circuitpython PATH]
                     [--fault swap|drop] [--keep DIR]

Makes a set of Ogg Vorbis files with ffmpeg's libvorbis (mono and stereo,
44.1 and 48 kHz, a few seconds and three minutes, low and high bitrates),
decodes each with ffmpeg to reference PCM, and decodes it again through
audiovorbis.VorbisDecoder on every interpreter named: this CPython always,
and MicroPython or CircuitPython builds that carry audiodsp when
--micropython or --circuitpython points at one. Nothing is stored
in the repository; the files are made fresh in a temporary directory.

A file passes when its frame count matches ffmpeg's exactly and every sample
is within TOLERANCE of ffmpeg's. Tremor decodes in fixed point and ffmpeg in
float, and the two land on 16 bits differently: Tremor shifts down, which
rounds towards minus infinity, and ffmpeg rounds to nearest. So about half of
all samples come out one step lower than ffmpeg's (the RMS difference is
0.71, the mean -0.50), and a handful elsewhere differ by 1 or 2; measured on
this set, none by more than 2. Beyond that, every interpreter must produce
the same bytes, because they run the same C.

--fault plants a defect in the decoder's output (vorbis_decode.py) and the run
must then FAIL: swap exchanges left and right, drop loses one block of 1024
frames part-way through. CI runs both first.

Needs ffmpeg with libvorbis on the PATH.
"""

import argparse
import array
import math
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent

#: The largest difference allowed between a decoded sample and ffmpeg's: one
#: step more than this set has ever shown.
TOLERANCE = 3
#: And the largest RMS difference over a whole file, in steps. A decoder that
#: rounded the way ffmpeg does would read 0; Tremor reads 0.71.
RMS_TOLERANCE = 1.0

# A signal with something for every part of the codec to do: a tone with
# vibrato, a sweep, a noise floor that keeps the high bitrates high, louder
# noise bursts and gaps of silence. The right channel differs from the left,
# so a swap shows.
LEFT = ("0.25*sin(2*PI*220*t+3*sin(2*PI*5*t))"
        "+0.15*sin(2*PI*(300+2000*mod(t,7)/7)*t)"
        "+0.04*(random(0)-0.5)"
        "+0.1*(random(0)-0.5)*gt(mod(t,3),2)")
RIGHT = ("(0.25*sin(2*PI*330*t)+0.1*sin(2*PI*1760*t)"
         "+0.04*(random(1)-0.5))*lt(mod(t,4),3.5)"
         "+0.12*(random(1)-0.5)*gt(mod(t,5),3)")

#: (name, channels, rate, seconds, libvorbis quality)
MATRIX = (
    ("mono_44k_short_low", 1, 44100, 4, -1),
    ("stereo_48k_short_high", 2, 48000, 4, 10),
    ("stereo_44k_long_low", 2, 44100, 180, 0),
    ("mono_48k_long_high", 1, 48000, 150, 8),
)


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args],
                   check=True)


def make(directory, name, channels, rate, seconds, quality):
    """The .ogg and ffmpeg's decode of it, as raw little-endian int16."""
    ogg = directory / (name + ".ogg")
    expr = LEFT if channels == 1 else LEFT + "|" + RIGHT
    ffmpeg("-f", "lavfi", "-i", "aevalsrc=exprs='%s':s=%d:d=%d" % (expr, rate, seconds),
           "-c:a", "libvorbis", "-q:a", str(quality), "-map_metadata", "-1",
           str(ogg))
    reference = directory / (name + ".ffmpeg.raw")
    ffmpeg("-i", str(ogg), "-f", "s16le", "-acodec", "pcm_s16le", str(reference))
    return ogg


def decode(argv, ogg, out, fault):
    command = argv + [str(HERE / "vorbis_decode.py"), str(ogg), str(out)]
    if fault:
        command += ["--fault", fault]
    environment = os.environ.copy()
    environment["MICROPYPATH"] = str(ROOT)
    result = subprocess.run(command, capture_output=True, text=True,
                            env=environment, check=False)
    if result.returncode != 0:
        raise RuntimeError("%s failed on %s:\n%s%s" % (argv[0], ogg.name,
                                                     result.stdout, result.stderr))
    rate, channels, frames = (int(x) for x in result.stdout.split()[-3:])
    return rate, channels, frames


def samples(path):
    data = array.array("h")
    data.frombytes(path.read_bytes())
    if sys.byteorder != "little":
        data.byteswap()
    return data


def compare(reference, decoded):
    """(max |difference|, RMS difference) over the common length."""
    worst, square = 0, 0
    for a, b in zip(reference, decoded):
        d = a - b
        if d < 0:
            d = -d
        if d > worst:
            worst = d
        square += d * d
    n = min(len(reference), len(decoded)) or 1
    return worst, math.sqrt(square / n)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--micropython", help="a MicroPython build with audiodsp in it")
    parser.add_argument("--circuitpython", help="a CircuitPython build with audiodsp in it")
    parser.add_argument("--fault", choices=("swap", "drop"))
    parser.add_argument("--keep", help="make the files here and leave them")
    args = parser.parse_args()
    if shutil.which("ffmpeg") is None:
        sys.exit("verify_vorbis needs ffmpeg with libvorbis on the PATH")

    interpreters = [("cpython", [sys.executable])]
    if args.micropython:
        interpreters.append(("micropython", [args.micropython]))
    if args.circuitpython:
        interpreters.append(("circuitpy", [args.circuitpython]))

    directory = Path(args.keep or tempfile.mkdtemp(prefix="verify_vorbis_"))
    directory.mkdir(parents=True, exist_ok=True)
    ok = True
    try:
        for name, channels, rate, seconds, quality in MATRIX:
            ogg = make(directory, name, channels, rate, seconds, quality)
            reference = samples(directory / (name + ".ffmpeg.raw"))
            kbps = ogg.stat().st_size * 8 / seconds / 1000
            outputs = []
            for label, argv in interpreters:
                out = directory / ("%s.%s.raw" % (name, label))
                got_rate, got_channels, frames = decode(argv, ogg, out, args.fault)
                decoded = samples(out)
                outputs.append(out.read_bytes())
                worst, rms = compare(reference, decoded)
                want_frames = len(reference) // channels
                passed = (got_rate == rate and got_channels == channels
                          and frames == want_frames
                          and worst <= TOLERANCE and rms <= RMS_TOLERANCE)
                print("%-22s %-11s %3d kbit/s  frames %8d/%-8d  max %5d  rms %7.3f  %s"
                      % (name, label, round(kbps), frames, want_frames, worst, rms,
                         "ok" if passed else "FAIL"))
                ok = ok and passed
            if len(set(outputs)) > 1:
                print("%-22s the interpreters decoded different bytes  FAIL" % name)
                ok = False
    finally:
        if not args.keep:
            shutil.rmtree(directory, ignore_errors=True)

    print("VORBIS REFERENCE %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
