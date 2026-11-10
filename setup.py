# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

import os
import shutil
import subprocess
import sys
from pathlib import Path

from setuptools import Extension, setup
from setuptools.command.build_py import build_py

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
        ["git", "-C", str(Path(__file__).parent), "describe", "--tags",
         "--match", "v[0-9]*", "--always", "--dirty", "--abbrev=7"],
        capture_output=True, text=True, check=True).stdout.strip() or "unknown"
except (OSError, subprocess.CalledProcessError):
    REVISION = "unknown"

HERE = Path(__file__).parent
MP3_DIR = HERE / ".deps" / "mp3" / "src"
# Helix's sources, as micropython.mk lists them. buffers.c is compiled through
# src/audiomp3/mp3_buffers.c, which hands it this extension's allocator.
MP3_SOURCES = ["bitstream.c", "dct32.c", "dequant.c", "dqchan.c", "huffman.c",
               "hufftabs.c", "imdct.c", "mp3dec.c", "mp3tabs.c", "polyphase.c",
               "scalfact.c", "stproc.c", "subband.c", "trigtabs.c"]


def fetch_mp3():
    """Put the pinned Adafruit_MP3 under .deps/mp3, with patches/adafruit_mp3/.

    What scripts/fetch_deps.sh does, in Python, so a build on Windows needs
    no shell. An sdist already carries the sources and skips this.
    """
    lock = (HERE / "DEPENDENCIES.lock").read_text().split("\n")
    name, url, rev = next(line.split() for line in lock
                          if line.strip() and not line.startswith("#"))
    dest = HERE / ".deps" / name

    def git(*args):
        subprocess.run(["git", "-C", str(dest), *args], check=True)

    if not (dest / ".git").is_dir():
        dest.mkdir(parents=True, exist_ok=True)
        git("init", "-q")
        git("remote", "add", "origin", url)
    git("fetch", "-q", "--depth", "1", "origin", rev)
    git("checkout", "-qf", rev)
    for patch in sorted((HERE / "patches" / "adafruit_mp3").glob("*.patch")):
        applied = subprocess.run(["git", "-C", str(dest), "apply", "--reverse", "--check", str(patch)],
                                 capture_output=True).returncode == 0
        if not applied:
            git("apply", str(patch))


# audiomp3 needs the MP3 decoder. AUDIODSP_OPTIONAL_DEPS=1 builds without it,
# as it does for the board builds; otherwise a failed fetch fails the build,
# so no wheel ships without the module.
MP3_EXTENSION = {}
if os.environ.get("AUDIODSP_OPTIONAL_DEPS") != "1":
    if not (MP3_DIR / "mp3dec.c").is_file():
        fetch_mp3()
    MP3_EXTENSION = {
        "sources": ["src/cpython/audiodsp_mp3.c", "src/audiomp3/mp3_buffers.c"]
        + [".deps/mp3/src/" + name for name in MP3_SOURCES],
        "include_dirs": [".deps/mp3/src"],
        # mp3dec.h stops at an unknown platform unless told to take the
        # portable one. On every platform a wheel is built for, that is the
        # path assembly.h already takes, and the one x86-64 boards' builds use.
        "define_macros": [("AUDIODSP_HAVE_MP3", "1"), ("MP3DEC_GENERIC", "1")],
    }

class build_py_with_stubs(build_py):
    """Install each module's type stub beside the module (PEP 561).

    Every wrapper module installs as a top-level ``audiocore.py`` and so on, so
    its stub goes in an ``audiocore-stubs`` folder, which type checkers search
    for. The folder's ``__init__.pyi`` is the one ``src/<module>/<module>.pyi``
    the repository keeps, copied at build time. Every ``src/*/*.pyi`` is taken,
    so a module that gains a stub ships it with no change here.
    """

    def run(self):
        super().run()
        for stub in sorted((HERE / "src").glob("*/*.pyi")):
            target = Path(self.build_lib) / (stub.stem + "-stubs")
            target.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(stub, target / "__init__.pyi")


setup(
    cmdclass={"build_py": build_py_with_stubs},
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
                "src/shared/audiodsp_cast.c",
                "src/shared/audiodsp_multiply.c",
                "src/shared/audiodsp_suboctave.c",
                "src/shared/audiodsp_feedback_delay.c",
                "src/shared/audiodsp_filter_f32.c",
                "src/shared/audiodsp_shaper.c",
                "src/shared/audiodsp_samplehold.c",
                "src/shared/audiodsp_drift.c",
                "src/shared/audiodsp_ladder.c",
                "src/shared/audiodsp_oscillator.c",
                "src/shared/audiodsp_trig.c",
                "src/shared/audiodsp_fft.c",
                "src/shared/audiodsp_convolve.c",
                "src/shared/audiodsp_tank.c",
                "src/shared/audiodsp_modal.c",
                "src/shared/audiodsp_sampler.c",
                "src/shared/audiodsp_meter.c",
                "src/shared/audiodsp_graph.c",
                "src/shared/audiodsp_graph_overdrive.c",
                # Xiph's Tremor, included into this one file from
                # third_party/tremor; docs/reference/vorbis.md.
                "src/shared/audiodsp_vorbis.c",
                # TensorFlow Lite Micro's microfrontend and kissfft, included
                # into this one file from third_party; audiofeatures' engine.
                "src/shared/audiodsp_microfrontend.c",
                "src/shared/audiodsp_features.c",
            ] + MP3_EXTENSION.get("sources", []),
            include_dirs=["src", "third_party/microfrontend", "third_party/kissfft"]
            + MP3_EXTENSION.get("include_dirs", []),
            define_macros=[("AUDIODSP_VERSION", '"%s"' % VERSION),
                           ("AUDIODSP_REVISION", '"%s"' % REVISION)]
            + MP3_EXTENSION.get("define_macros", []),
            extra_compile_args=MACOS_COMPILE_ARGS,
        )
    ]
)
