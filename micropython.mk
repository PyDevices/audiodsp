# MicroPython Make-based build glue for audioif (unix, windows).
# For CMake-based ports (esp32, rp2, …), see micropython.cmake in this dir.
#
# Discovered via USER_C_MODULES pointing at the workspace directory that
# contains this repo (its parent) — see the parent workspace's own build
# script.

MPAUDIO_MOD_DIR := $(USERMOD_DIR)
MPAUDIO_SRC_DIR := $(MPAUDIO_MOD_DIR)/src

CFLAGS_USERMOD += -I$(MPAUDIO_SRC_DIR)

# --- which audioif this firmware was built from -----------------------------
#
# audioif#55. `os.uname().version` answers for MicroPython and says nothing
# about us, so a board digest that moves cannot be attributed to a revision.
# Computed HERE, from the directory the build is actually reading, because that
# is the only place that knows: on 2026-09-09 a firmware had been built from a
# throwaway source tree during a pin move, and the commit was not recoverable
# from the working tree or from the .bin files left on disk.
#
# `--always --dirty` so a tree with uncommitted changes says so. Both default
# to "unknown" rather than guessing if git is absent or this is a tarball --
# see src/cp_compat/audioif_build.c.
MPAUDIO_VERSION := $(shell cat $(MPAUDIO_MOD_DIR)/VERSION 2>/dev/null || echo 0.0.0+unknown)
MPAUDIO_REVISION := $(shell git -C $(MPAUDIO_MOD_DIR) describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)
CFLAGS_USERMOD += -DAUDIOIF_VERSION='"$(MPAUDIO_VERSION)"'
CFLAGS_USERMOD += -DAUDIOIF_REVISION='"$(MPAUDIO_REVISION)"'

# --- cp_compat: small shims for CircuitPython-only helpers (mp_arg_validate_*,
#     cp_enum, MP_PROPERTY_GETTER/GETSET, default___enter__/__exit__) that the
#     ported CircuitPython audio/synthio sources call directly. See
#     docs/porting-plan.md tier 0 and src/cp_compat/*.h for provenance.
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/cp_compat/audioif_build.c \
    $(MPAUDIO_SRC_DIR)/cp_compat/argcheck.c \
    $(MPAUDIO_SRC_DIR)/cp_compat/enum.c \
    $(MPAUDIO_SRC_DIR)/cp_compat/context_manager_helpers.c \
    $(MPAUDIO_SRC_DIR)/cp_compat/proto.c \
    $(MPAUDIO_SRC_DIR)/cp_compat/util.c \
    $(MPAUDIO_SRC_DIR)/cp_compat/objproperty.c \
    $(MPAUDIO_SRC_DIR)/cp_compat/namedtuple.c

# --- runtime-neutral sample protocol/state shared with the CPython wheel
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/shared/audioif_sample.c \
    $(MPAUDIO_SRC_DIR)/shared/audioif_pump_lock.c \
    $(MPAUDIO_SRC_DIR)/shared/audioif_rawsample.c \
    $(MPAUDIO_SRC_DIR)/shared/audioif_synth_dsp.c \
    $(MPAUDIO_SRC_DIR)/shared/audioif_envelope.c \
    $(MPAUDIO_SRC_DIR)/shared/audioif_distortion.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_biquad.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_echo.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_phaser.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_chorus.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_flanger.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_granular_pitch_shift.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_multitap.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_pitchshift.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_freeverb.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_dynamics.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_splitter.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_midside.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_remix.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_multiply.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_suboctave.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_feedback_delay.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_filter_f32.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_shaper.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_samplehold.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_ladder.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_trig.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_fft.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_convolve.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_tank.c
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/shared/audioif_modal.c

# --- ulab (numpy-alike): cloned sibling dependency, pinned to match this
#     workspace's CircuitPython checkout (see docs/porting-plan.md). Its own
#     code/micropython.mk expects USERMOD_DIR to point at ulab/code; the
#     workspace CMake aggregator finds ulab/code/micropython.cmake on its
#     own (mindepth 2/maxdepth 3), but py.mk's Make-port loop only globs
#     $(USER_C_MODULES)/*/micropython.mk at depth 1, so chain it in here.
#     USERMOD_DIR is restored below since ulab's makefile does not save it.
#     Search order: the repo-local .deps/ tree (scripts/fetch_deps.sh,
#     pinned by DEPENDENCIES.lock) first, then the workspace sibling.
MPAUDIO_ULAB_CODE_DIR := $(abspath $(MPAUDIO_MOD_DIR)/.deps/ulab/code)
ifeq ($(wildcard $(MPAUDIO_ULAB_CODE_DIR)/ulab.c),)
MPAUDIO_ULAB_CODE_DIR := $(abspath $(MPAUDIO_MOD_DIR)/../ulab/code)
endif
ifeq ($(wildcard $(MPAUDIO_ULAB_CODE_DIR)/ulab.c),)
ifneq ($(AUDIOIF_OPTIONAL_DEPS),1)
$(error audioif: ulab not found (looked in .deps/ulab and ../ulab). Run ./scripts/fetch_deps.sh, or set AUDIOIF_OPTIONAL_DEPS=1 to build without it)
endif
endif
ifneq ($(wildcard $(MPAUDIO_ULAB_CODE_DIR)/ulab.c),)
USERMOD_DIR := $(MPAUDIO_ULAB_CODE_DIR)
include $(MPAUDIO_ULAB_CODE_DIR)/micropython.mk
USERMOD_DIR := $(MPAUDIO_MOD_DIR)

# `import ulab.numpy` / `import ulab.scipy` are dotted submodule imports of a
# built-in (ulab's globals dict has `numpy`/`scipy` as plain module-typed
# entries, same shape CircuitPython relies on for its own dotted built-ins).
# Mainline only walks that path when MICROPY_MODULE_BUILTIN_SUBPACKAGES is
# on (py/builtinimport.c, process_import_at_level); off by default outside
# the "everything" ROM level. CFLAGS_EXTRA reaches every compiled object for
# this port (see ports/unix/Makefile), not just usermod sources, so this
# turns it on build-wide -- confirmed needed by testing `import ulab.numpy`
# against this exact ulab pin on the unix standard variant.
override CFLAGS_EXTRA += -DMICROPY_MODULE_BUILTIN_SUBPACKAGES=1
endif

# --- module skeleton: empty `import <name>` targets for every top-level
#     module this tree will grow into (see docs/porting-plan.md). Tiers add
#     real classes/functions by editing mpaudio_modules.c's globals tables
#     directly, so this stays the single source of each module's contents.
SRC_USERMOD_C += $(MPAUDIO_SRC_DIR)/mpaudio_modules.c

# --- tier 1: audiocore (the audiosample protocol, RawSample, WaveFile) ---
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audiocore/__init__.c \
    $(MPAUDIO_SRC_DIR)/audiocore/RawSample.c \
    $(MPAUDIO_SRC_DIR)/audiocore/WaveFile.c \
    $(MPAUDIO_SRC_DIR)/audiocore/module.c

# --- tier 2: synthio (block layer, Math, LFO, Biquad, Note, Synthesizer,
#     MidiTrack) ---
#
# CIRCUITPY_SYNTHIO_MAX_CHANNELS: src/synthio/__init__.h defaults this to 2
# (CP's own conservative default -- see the comment there), which caps
# concurrent Notes across an entire Synthesizer, not per-key. Any
# realistic polyphonic patch blows past 2 immediately (two notes held on a
# 2-oscillator/detuned voice is already 4 concurrent Notes), and the excess
# is silently REFUSED -- not stolen, not truncated. Corrected 2026-09-02
# (audioif#14): this comment used to say "silently stealing/truncating",
# which describes a gentler failure than the one that happens.
# find_channel_with_note (src/synthio/__init__.c:361) scans only channels
# where the note is NOT playing and takes the quietest, so it reclaims from
# RELEASED notes only; with every channel held it returns -1 and
# synthio_span_change_note drops the press at :425. The difference matters:
# a stolen note is one you hear cut short, a refused note never sounds at
# all, and only the second is inaudible as a fault. Upstream
# handles this per-port (ports/raspberrypi/mpconfigport.mk: 24,
# ports/nordic: 12, ...); this workspace's CircuitPython parity oracle
# (the parent workspace's circuitpython checkout, unix `coverage` variant,
# bin/circuitpython) is itself built with 14
# (ports/unix/variants/coverage/mpconfigvariant.mk).
# so this file matched that until 2026-09-03, when the ceiling moved to 64
# (#31). The oracle stays at 14 and is not rebuilt, so material that crosses
# 14 channels is non-parity by construction and declared as such in
# docs/upstream-diff.md. Everything under 14 channels still byte-matches: the
# mix-down limiter only reshapes samples past +/-28000, and all four parity
# gates are byte-identical at 14, 36 and 56 (measured).
#
# CORRECTION 2026-09-03: this comment used to say "unix/windows/wasm (every
# port this Make-based file covers)". That is wrong about its own reach and
# had been since it was written. py/py.mk globs every usermod .mk file in,
# so this line lands on EVERY Make port -- verified: samd, stm32, nrf,
# mimxrt, esp8266, renesas-ra, alif, cc3200, rp2, unix, windows,
# webassembly. It is not the desktop file; it is the default for everything
# not built through CMake, SAMD51 audio boards included. That mattered when
# this said 14 and CMake said 8, because a proposal to raise "desktop" here
# would silently have raised those mcu boards too. It matters less now that
# all three paths read 14 on purpose, but the comment should be true.
CFLAGS_USERMOD += -DCIRCUITPY_SYNTHIO_MAX_CHANNELS=64
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/synthio/__init__.c \
    $(MPAUDIO_SRC_DIR)/synthio/Math.c \
    $(MPAUDIO_SRC_DIR)/synthio/LFO.c \
    $(MPAUDIO_SRC_DIR)/synthio/Biquad.c \
    $(MPAUDIO_SRC_DIR)/synthio/Note.c \
    $(MPAUDIO_SRC_DIR)/synthio/Synthesizer.c \
    $(MPAUDIO_SRC_DIR)/synthio/MidiTrack.c

# --- tier 3: audiomixer (Mixer, MixerVoice) ---
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audiomixer/Mixer.c \
    $(MPAUDIO_SRC_DIR)/audiomixer/MixerVoice.c \
    $(MPAUDIO_SRC_DIR)/audiomixer/module.c

# --- tier 4: effects (audiospeed) ---
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audiospeed/Resampler.c \
    $(MPAUDIO_SRC_DIR)/audiospeed/SpeedChanger.c \
    $(MPAUDIO_SRC_DIR)/audiospeed/module.c

# --- tier 4: effects (audiofreeverb) ---
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audiofreeverb/Freeverb.c \
    $(MPAUDIO_SRC_DIR)/audiofreeverb/module.c

# --- tier 4: effects (audiofilters) ---
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audiofilters/__init__.c \
    $(MPAUDIO_SRC_DIR)/audiofilters/Filter.c \
    $(MPAUDIO_SRC_DIR)/audiofilters/Distortion.c \
    $(MPAUDIO_SRC_DIR)/audiofilters/Phaser.c \
    $(MPAUDIO_SRC_DIR)/audiofilters/module.c

# --- tier 4: effects (audiodelays) ---
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audiodelays/Chorus.c \
    $(MPAUDIO_SRC_DIR)/audiodelays/Echo.c \
    $(MPAUDIO_SRC_DIR)/audiodelays/Flanger.c \
    $(MPAUDIO_SRC_DIR)/audiodelays/GranularPitchShift.c \
    $(MPAUDIO_SRC_DIR)/audiodelays/MultiTapDelay.c \
    $(MPAUDIO_SRC_DIR)/audiodelays/PitchShift.c \
    $(MPAUDIO_SRC_DIR)/audiodelays/module.c

# --- tier 6: audiodynamics (Dynamics, DYN_*), audioroute (Splitter,
#     MidSide), audiomath (Multiply, SubOctave), audioecho
#     (FeedbackDelay), audioladder (Ladder), audioconvolve (Convolver),
#     audiobiquad (Biquad, AllPass), audioverb (Tank) and
#     audiomodal (Bank) ---
#
# The modules here are not CircuitPython ports. The first two come from
# micropython-vst3's `vstaudio` usermod, which grew them for its effects
# library; the rest are audioif's own -- audiomath is the only way to modulate
# a stream at audio rate and the only way to divide one down in frequency,
# audioecho puts a filter inside a delay's feedback loop, audioladder puts
# four one-poles and a saturator inside a filter's, audioconvolve applies a
# measured impulse response rather than imitating one, audiobiquad
# filters in float so a tail reaches exact zero, which neither ported
# kernel can (audioif#23, #36), and audioverb is a reverberation tank whose
# line lengths and output taps come from Python rather than being compiled
# in the way audiofreeverb's are, and audiomodal sums a bank of
# resonators before the quantiser, which a chain of audiobiquads cannot
# do at any width (audioroute.Splitter stops at four taps) or any depth
# (every node in the chain quantises, and a quiet mode is then noise).
# See docs/upstream-diff.md.
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audiodynamics/Dynamics.c \
    $(MPAUDIO_SRC_DIR)/audiodynamics/module.c
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audioroute/MidSide.c \
    $(MPAUDIO_SRC_DIR)/audioroute/Port.c \
    $(MPAUDIO_SRC_DIR)/audioroute/Splitter.c \
    $(MPAUDIO_SRC_DIR)/audioroute/SplitterTap.c \
    $(MPAUDIO_SRC_DIR)/audioroute/module.c
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audiomath/Multiply.c \
    $(MPAUDIO_SRC_DIR)/audiomath/SubOctave.c \
    $(MPAUDIO_SRC_DIR)/audiomath/module.c
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audioecho/FeedbackDelay.c \
    $(MPAUDIO_SRC_DIR)/audioecho/module.c
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audioshaper/Waveshaper.c \
    $(MPAUDIO_SRC_DIR)/audioshaper/SampleHold.c \
    $(MPAUDIO_SRC_DIR)/audioshaper/module.c
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audioladder/Ladder.c \
    $(MPAUDIO_SRC_DIR)/audioladder/module.c
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audioconvolve/Convolver.c \
    $(MPAUDIO_SRC_DIR)/audioconvolve/module.c
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audiobiquad/Biquad.c \
    $(MPAUDIO_SRC_DIR)/audiobiquad/AllPass.c \
    $(MPAUDIO_SRC_DIR)/audiobiquad/module.c
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audioverb/Tank.c \
    $(MPAUDIO_SRC_DIR)/audioverb/module.c
SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audiomodal/Bank.c \
    $(MPAUDIO_SRC_DIR)/audiomodal/module.c

# --- tier 5: audiomp3 (MP3Decoder) ---
#
# lib/mp3: a cloned sibling dependency in the parent workspace (the upstream
# adafruit/Adafruit_MP3 repo, pinned to the exact commit the parent
# workspace's circuitpython checkout vendors in its own lib/mp3), not a
# port -- same treatment as ulab above. Its `src/` is the Helix
# fixed-point MP3 decoder (RealNetworks, 2003), under RPSL 1.0/RCSL 1.0
# per-file headers, NOT MIT; CircuitPython itself carries it unmodified
# under those terms rather than relicensing it, and so does this port --
# see docs/upstream-diff.md, "Tier 5 audiomp3: license" for the full check.
# One local patch was needed on top of the pinned commit
# (mp3/src/assembly.h in that sibling clone): its MSVC-only inline-asm branch
# guards on `defined _WIN32`, which mingw-w64 GCC (this workspace's Windows
# MicroPython target) also defines, so it now also excludes __GNUC__ and
# falls through to the portable C path instead -- CircuitPython itself never
# hits this, since it has no Windows port. See docs/upstream-diff.md for the
# full patch rationale.
# mp3dec.h picks its platform-specific fixed-point/asm path by matching a
# closed list of (__GNUC__ + arch) combinations, with an explicit
# MP3DEC_GENERIC escape hatch for anything else -- `#error No platform
# defined` otherwise. Emscripten/wasm32 matches none of the listed
# combinations (it's not __i386__/__amd64__/ARM), so it needs that escape
# hatch; unix (__amd64__) and windows/mingw-w64 (__i386__ or __amd64__,
# already covered by the same __GNUC__ branches unix uses) both match a
# named branch already and must NOT get this -- it would silently swap
# their working platform-specific path for the portable fallback. Detect
# the wasm port the same way the parent workspace's wasmbridge module does,
# since there's no other reliable way to ask "which port is this" from inside a
# Make port's shared USER_C_MODULES glue.
MPAUDIO_WASM_PORT := $(findstring /ports/webassembly,$(abspath $(CURDIR)))
ifneq ($(MPAUDIO_WASM_PORT),)
CFLAGS_USERMOD += -DMP3DEC_GENERIC
endif

MPAUDIO_MP3_SRC_DIR := $(abspath $(MPAUDIO_MOD_DIR)/.deps/mp3/src)
ifeq ($(wildcard $(MPAUDIO_MP3_SRC_DIR)/mp3dec.c),)
MPAUDIO_MP3_SRC_DIR := $(abspath $(MPAUDIO_MOD_DIR)/../mp3/src)
endif
ifeq ($(wildcard $(MPAUDIO_MP3_SRC_DIR)/mp3dec.c),)
ifneq ($(AUDIOIF_OPTIONAL_DEPS),1)
$(error audioif: Adafruit_MP3 not found (looked in .deps/mp3 and ../mp3). Run ./scripts/fetch_deps.sh, or set AUDIOIF_OPTIONAL_DEPS=1 to build without audiomp3)
endif
endif
ifneq ($(wildcard $(MPAUDIO_MP3_SRC_DIR)/mp3dec.c),)
CFLAGS_USERMOD += -I$(MPAUDIO_MP3_SRC_DIR)
SRC_USERMOD_C += $(addprefix $(MPAUDIO_MP3_SRC_DIR)/, \
    bitstream.c \
    buffers.c \
    dct32.c \
    dequant.c \
    dqchan.c \
    huffman.c \
    hufftabs.c \
    imdct.c \
    mp3dec.c \
    mp3tabs.c \
    polyphase.c \
    scalfact.c \
    stproc.c \
    subband.c \
    trigtabs.c \
    )

# buffers.c's MPDEC_ALLOCATOR(x)/MPDEC_FREE(x) macros (used nowhere else in
# lib/mp3) route through this port's own mp3_alloc()/mp3_free()
# (src/audiomp3/mp3_alloc.h -> m_malloc_maybe/m_free), matching upstream's
# own production (non-coverage-build) allocator wiring exactly, including
# the -fwrapv upstream applies to this one file only (Helix's fixed-point
# DSP relies on defined signed-overflow wraparound here).
$(BUILD)/mp3/src/buffers.o: CFLAGS += -include "$(MPAUDIO_SRC_DIR)/audiomp3/mp3_alloc.h" -D'MPDEC_ALLOCATOR(x)=mp3_alloc(x)' -D'MPDEC_FREE(x)=mp3_free(x)' -fwrapv

SRC_USERMOD_C += \
    $(MPAUDIO_SRC_DIR)/audiomp3/mp3_alloc.c \
    $(MPAUDIO_SRC_DIR)/audiomp3/MP3Decoder.c \
    $(MPAUDIO_SRC_DIR)/audiomp3/module.c
endif
