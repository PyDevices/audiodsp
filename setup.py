import subprocess
import sys
from pathlib import Path

from setuptools import Extension, setup

# The parity gates hash PCM bit-exactly against the CircuitPython oracle,
# and Linux (gcc) and Windows (MSVC) builds agree with it. On macOS arm64,
# clang's default -ffp-contract=on fuses multiply-adds into fmadd (baseline
# on AArch64, so contraction actually happens there, unlike x86-64 without
# -mfma), which perturbs last-ulp float results and broke verify_effects.
# Scoped to macOS the way the single-precision cell scoped its own flags
# (clean-build.yml): turn contraction off so every shipped wheel computes
# the same bits the oracle blessed.
MACOS_COMPILE_ARGS = ["-ffp-contract=off"] if sys.platform == "darwin" else []

# _audiodsp.__version__ used to be a literal in the C, and drifted from VERSION
# the first time VERSION moved. There is one version here, and it is this file.
VERSION = Path(__file__).parent.joinpath("VERSION").read_text().strip()

# The commit this wheel was built from, for `_audiodsp.__revision__`. The board
# builds compute the same thing in micropython.mk / micropython.cmake; see
# src/cp_compat/audiodsp_build.h for why it is computed and never stored, and why
# "unknown" is the honest answer outside a checkout rather than an error.
try:
    REVISION = subprocess.run(
        ["git", "-C", str(Path(__file__).parent), "describe", "--always",
         "--dirty", "--abbrev=7"],
        capture_output=True, text=True, check=True).stdout.strip() or "unknown"
except (OSError, subprocess.CalledProcessError):
    REVISION = "unknown"

setup(
    ext_modules=[
        Extension(
            "_audiodsp",
            sources=[
                "src/cpython/_audiodsp.c",
                "src/shared/audiodsp_sample.c",
                "src/shared/audiodsp_port.c",
                "src/shared/audiodsp_pump_lock.c",
                "src/shared/audiodsp_rawsample.c",
                "src/shared/audiodsp_synth_dsp.c",
                "src/shared/audiodsp_envelope.c",
                "src/shared/audiodsp_distortion.c",
                "src/shared/audiodsp_biquad.c",
                "src/shared/audiodsp_echo.c",
                "src/shared/audiodsp_phaser.c",
                "src/shared/audiodsp_chorus.c",
                "src/shared/audiodsp_multitap.c",
                "src/shared/audiodsp_pitchshift.c",
                "src/shared/audiodsp_freeverb.c",
                "src/shared/audiodsp_dynamics.c",
                "src/shared/audiodsp_splitter.c",
                "src/shared/audiodsp_midside.c",
                "src/shared/audiodsp_remix.c",
                "src/shared/audiodsp_multiply.c",
                "src/shared/audiodsp_suboctave.c",
                "src/shared/audiodsp_feedback_delay.c",
                "src/shared/audiodsp_filter_f32.c",
                "src/shared/audiodsp_shaper.c",
                "src/shared/audiodsp_samplehold.c",
                "src/shared/audiodsp_ladder.c",
                "src/shared/audiodsp_trig.c",
                "src/shared/audiodsp_fft.c",
                "src/shared/audiodsp_convolve.c",
                "src/shared/audiodsp_tank.c",
                "src/shared/audiodsp_modal.c",
            ],
            include_dirs=["src"],
            define_macros=[("AUDIODSP_VERSION", '"%s"' % VERSION),
                           ("AUDIODSP_REVISION", '"%s"' % REVISION)],
            extra_compile_args=MACOS_COMPILE_ARGS,
        )
    ]
)
