# MicroPython CMake glue for audioif (esp32, rp2, …).
# For Make-based ports (unix, windows), see micropython.mk in this dir.
#
# Point USER_C_MODULES at this repo (or this file) directly, e.g.:
#   idf.py build -DUSER_C_MODULES=<path to audioif>
# Or let the parent workspace's own aggregator discover it alongside
# other usermods.

set(MPAUDIO_MOD_DIR ${CMAKE_CURRENT_LIST_DIR})
set(MPAUDIO_SRC_DIR ${MPAUDIO_MOD_DIR}/src)

add_library(usermod_mpaudio INTERFACE)

# --- cp_compat: see micropython.mk for provenance/rationale (identical
#     source list, kept in sync by hand -- both build flavors compile the
#     same C files).
target_sources(usermod_mpaudio INTERFACE
    ${MPAUDIO_SRC_DIR}/cp_compat/audioif_build.c
    ${MPAUDIO_SRC_DIR}/cp_compat/argcheck.c
    ${MPAUDIO_SRC_DIR}/cp_compat/enum.c
    ${MPAUDIO_SRC_DIR}/cp_compat/context_manager_helpers.c
    ${MPAUDIO_SRC_DIR}/cp_compat/proto.c
    ${MPAUDIO_SRC_DIR}/cp_compat/util.c
    ${MPAUDIO_SRC_DIR}/cp_compat/objproperty.c
    ${MPAUDIO_SRC_DIR}/cp_compat/namedtuple.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_sample.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_rawsample.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_synth_dsp.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_envelope.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_distortion.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_biquad.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_echo.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_phaser.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_chorus.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_flanger.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_granular_pitch_shift.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_multitap.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_pitchshift.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_freeverb.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_dynamics.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_splitter.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_midside.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_remix.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_multiply.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_suboctave.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_feedback_delay.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_filter_f32.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_shaper.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_ladder.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_trig.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_fft.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_convolve.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_tank.c
    ${MPAUDIO_SRC_DIR}/shared/audioif_modal.c
    ${MPAUDIO_SRC_DIR}/mpaudio_modules.c
    ${MPAUDIO_SRC_DIR}/audiocore/__init__.c
    ${MPAUDIO_SRC_DIR}/audiocore/RawSample.c
    ${MPAUDIO_SRC_DIR}/audiocore/WaveFile.c
    ${MPAUDIO_SRC_DIR}/audiocore/module.c
    ${MPAUDIO_SRC_DIR}/synthio/__init__.c
    ${MPAUDIO_SRC_DIR}/synthio/Math.c
    ${MPAUDIO_SRC_DIR}/synthio/LFO.c
    ${MPAUDIO_SRC_DIR}/synthio/Biquad.c
    ${MPAUDIO_SRC_DIR}/synthio/Note.c
    ${MPAUDIO_SRC_DIR}/synthio/Synthesizer.c
    ${MPAUDIO_SRC_DIR}/synthio/MidiTrack.c
    ${MPAUDIO_SRC_DIR}/audiomixer/Mixer.c
    ${MPAUDIO_SRC_DIR}/audiomixer/MixerVoice.c
    ${MPAUDIO_SRC_DIR}/audiomixer/module.c
    ${MPAUDIO_SRC_DIR}/audiospeed/Resampler.c
    ${MPAUDIO_SRC_DIR}/audiospeed/SpeedChanger.c
    ${MPAUDIO_SRC_DIR}/audiospeed/module.c
    ${MPAUDIO_SRC_DIR}/audiofreeverb/Freeverb.c
    ${MPAUDIO_SRC_DIR}/audiofreeverb/module.c
    ${MPAUDIO_SRC_DIR}/audiofilters/__init__.c
    ${MPAUDIO_SRC_DIR}/audiofilters/Filter.c
    ${MPAUDIO_SRC_DIR}/audiofilters/Distortion.c
    ${MPAUDIO_SRC_DIR}/audiofilters/Phaser.c
    ${MPAUDIO_SRC_DIR}/audiofilters/module.c
    ${MPAUDIO_SRC_DIR}/audiodelays/Chorus.c
    ${MPAUDIO_SRC_DIR}/audiodelays/Echo.c
    ${MPAUDIO_SRC_DIR}/audiodelays/Flanger.c
    ${MPAUDIO_SRC_DIR}/audiodelays/GranularPitchShift.c
    ${MPAUDIO_SRC_DIR}/audiodelays/MultiTapDelay.c
    ${MPAUDIO_SRC_DIR}/audiodelays/PitchShift.c
    ${MPAUDIO_SRC_DIR}/audiodelays/module.c
    ${MPAUDIO_SRC_DIR}/audiodynamics/Dynamics.c
    ${MPAUDIO_SRC_DIR}/audiodynamics/module.c
    ${MPAUDIO_SRC_DIR}/audioroute/MidSide.c
    ${MPAUDIO_SRC_DIR}/audioroute/Splitter.c
    ${MPAUDIO_SRC_DIR}/audioroute/SplitterTap.c
    ${MPAUDIO_SRC_DIR}/audioroute/module.c
    ${MPAUDIO_SRC_DIR}/audiomath/Multiply.c
    ${MPAUDIO_SRC_DIR}/audiomath/SubOctave.c
    ${MPAUDIO_SRC_DIR}/audiomath/module.c
    ${MPAUDIO_SRC_DIR}/audioecho/FeedbackDelay.c
    ${MPAUDIO_SRC_DIR}/audioecho/module.c
    ${MPAUDIO_SRC_DIR}/audioshaper/Waveshaper.c
    ${MPAUDIO_SRC_DIR}/audioshaper/module.c
    ${MPAUDIO_SRC_DIR}/audioladder/Ladder.c
    ${MPAUDIO_SRC_DIR}/audioladder/module.c
    ${MPAUDIO_SRC_DIR}/audioconvolve/Convolver.c
    ${MPAUDIO_SRC_DIR}/audioconvolve/module.c
    ${MPAUDIO_SRC_DIR}/audiobiquad/Biquad.c
    ${MPAUDIO_SRC_DIR}/audiobiquad/AllPass.c
    ${MPAUDIO_SRC_DIR}/audiobiquad/module.c
    ${MPAUDIO_SRC_DIR}/audioverb/Tank.c
    ${MPAUDIO_SRC_DIR}/audioverb/module.c
    ${MPAUDIO_SRC_DIR}/audiomodal/Bank.c
    ${MPAUDIO_SRC_DIR}/audiomodal/module.c
)

target_include_directories(usermod_mpaudio INTERFACE ${MPAUDIO_SRC_DIR})

# --- tier 5: audiomp3 (MP3Decoder) ---
#
# See micropython.mk for the full rationale (license, the cloned mp3
# sibling in the parent workspace, the allocator wiring, the mp3dec.h
# platform-detection escape hatch) -- only the CMake-specific parts
# (target-arch detection, per-file compile flags) are re-explained here.
#
# mp3dec.h's closed (__GNUC__, arch) list covers ARM (both `ARM` and
# `__ARMEL__`) and x86 (`__i386__`/`__amd64__`) but nothing else -- crucially,
# neither Xtensa nor RISC-V is in it, so *every* esp32 target this workspace
# builds (Xtensa: esp32/esp32s2/esp32s3; RISC-V: esp32c2/c3/c5/c6/p4) needs
# the `MP3DEC_GENERIC` escape hatch, while rp2 (RP2040/RP2350, both ARM
# Cortex-M, matching the `__GNUC__ && __ARMEL__` branch natively) does not.
# `CONFIG_IDF_TARGET_ARCH_RISCV`/`_ARCH_XTENSA` are the sdkconfig-derived
# CMake variables ESP-IDF's own esp32_common.cmake uses for this exact same
# arch branch (its `MICROPY_CROSS_FLAGS` selection) -- reused here instead
# of re-deriving "which target is this" from scratch; both are reliably set
# by the time usermod.cmake runs. rp2 has no such variable (native ARM
# match, no define needed). If a future non-ARM, non-x86 CMake port shows
# up, add its own arch-detection variable to this same `if()`.
if(CONFIG_IDF_TARGET_ARCH_RISCV OR CONFIG_IDF_TARGET_ARCH_XTENSA)
    target_compile_definitions(usermod_mpaudio INTERFACE MP3DEC_GENERIC)
    # The real compile picks this up fine via the usermod_mpaudio ->
    # usermod -> MICROPY_TARGET INTERFACE chain (ninja resolves transitive
    # usage requirements) -- but the QSTR-extraction preprocessing pass
    # (py/mkrules.cmake's qstr.i.last rule, ~line 63) builds its own flag
    # list via a raw `get_target_property(... MICROPY_TARGET
    # COMPILE_DEFINITIONS)`, which does NOT walk linked INTERFACE libraries
    # the way an actual compile does -- confirmed the hard way: that pass's
    # actual `-E` invocation was missing MP3DEC_GENERIC even though the
    # object-compile rule's DEFINES had it, and hit mp3dec.h's `#error`
    # directly. py.cmake's own `micropy_gather_target_properties()` exists
    # to bridge exactly this INTERFACE-library gap, but esp32_common.cmake
    # only calls it over `__COMPONENT_NAMES_RESOLVED` (registered ESP-IDF
    # components) -- "usermod" is a plain CMake target inside the main
    # component, not a component itself, so that loop never reaches it.
    # `MICROPY_CPP_DEF_EXTRA` is the accumulator both that gather-loop and
    # mkrules.cmake's own qstr flag-building read from (mkrules.cmake:68);
    # appending to it directly, here, is a normal (non-cache) variable
    # write in the same include()-preserved scope mkrules.cmake later reads
    # from -- the portable fix, not an esp32-specific one.
    list(APPEND MICROPY_CPP_DEF_EXTRA MP3DEC_GENERIC)
endif()

set(MPAUDIO_MP3_SRC_DIR ${MPAUDIO_MOD_DIR}/.deps/mp3/src)
if(NOT EXISTS ${MPAUDIO_MP3_SRC_DIR}/mp3dec.c)
    set(MPAUDIO_MP3_SRC_DIR ${MPAUDIO_MOD_DIR}/../mp3/src)
endif()
if(NOT EXISTS ${MPAUDIO_MP3_SRC_DIR}/mp3dec.c AND NOT DEFINED ENV{AUDIOIF_OPTIONAL_DEPS})
    message(FATAL_ERROR "audioif: Adafruit_MP3 not found (.deps/mp3 or ../mp3). Run ./scripts/fetch_deps.sh, or set AUDIOIF_OPTIONAL_DEPS=1.")
endif()
if(EXISTS ${MPAUDIO_MP3_SRC_DIR}/mp3dec.c)
    target_include_directories(usermod_mpaudio INTERFACE ${MPAUDIO_MP3_SRC_DIR})
    target_sources(usermod_mpaudio INTERFACE
        ${MPAUDIO_MP3_SRC_DIR}/bitstream.c
        ${MPAUDIO_MP3_SRC_DIR}/buffers.c
        ${MPAUDIO_MP3_SRC_DIR}/dct32.c
        ${MPAUDIO_MP3_SRC_DIR}/dequant.c
        ${MPAUDIO_MP3_SRC_DIR}/dqchan.c
        ${MPAUDIO_MP3_SRC_DIR}/huffman.c
        ${MPAUDIO_MP3_SRC_DIR}/hufftabs.c
        ${MPAUDIO_MP3_SRC_DIR}/imdct.c
        ${MPAUDIO_MP3_SRC_DIR}/mp3dec.c
        ${MPAUDIO_MP3_SRC_DIR}/mp3tabs.c
        ${MPAUDIO_MP3_SRC_DIR}/polyphase.c
        ${MPAUDIO_MP3_SRC_DIR}/scalfact.c
        ${MPAUDIO_MP3_SRC_DIR}/stproc.c
        ${MPAUDIO_MP3_SRC_DIR}/subband.c
        ${MPAUDIO_MP3_SRC_DIR}/trigtabs.c
        ${MPAUDIO_SRC_DIR}/audiomp3/mp3_alloc.c
        ${MPAUDIO_SRC_DIR}/audiomp3/MP3Decoder.c
        ${MPAUDIO_SRC_DIR}/audiomp3/module.c
    )

    # buffers.c's MPDEC_ALLOCATOR(x)/MPDEC_FREE(x) (used nowhere else in
    # lib/mp3) route through this port's own mp3_alloc()/mp3_free() -- same
    # -include + -fwrapv treatment as micropython.mk's buffers.o rule.
    set_source_files_properties(${MPAUDIO_MP3_SRC_DIR}/buffers.c PROPERTIES
        COMPILE_OPTIONS "-include;${MPAUDIO_SRC_DIR}/audiomp3/mp3_alloc.h;-DMPDEC_ALLOCATOR(x)=mp3_alloc(x);-DMPDEC_FREE(x)=mp3_free(x);-fwrapv"
    )
endif()

# See micropython.mk for why this is needed for `import ulab.numpy` (and any
# future dotted submodule this tree grows) to work at all.
target_compile_definitions(usermod_mpaudio INTERFACE MICROPY_MODULE_BUILTIN_SUBPACKAGES=1)

# CIRCUITPY_SYNTHIO_MAX_CHANNELS (phase 8e decision, made while actually
# building the ESP32-P4 firmware -- see docs/porting-plan.md "8e"): the
# header default of 2 is not just conservative, it is *broken* for
# essentially any real patch -- confirmed in phase 7/8d, two notes held on
# a plain 2-oscillator/detuned voice is already 4 concurrent Notes, and the
# excess is silently REFUSED rather than erroring. Corrected 2026-09-02
# (audioif#14): this said "silently steals/truncates", which is the wrong
# failure. find_channel_with_note (src/synthio/__init__.c:361) reclaims only
# from RELEASED channels; when every channel is held the press is dropped
# outright (:425). A note that never sounds is the one you cannot hear go
# wrong. CircuitPython's
# own more-capable boards all raise it well past 2 (raspberrypi: 24,
# nordic/mimxrt10xx: 12) -- 2 is really only survivable on the most
# memory-starved boards, not a sane default for anything with real RAM.
# Raised 8 -> 14, then 14 -> 64 the same day (#31): 14 fits the drum kits,
# 64 fits a ten-finger chord on the melodic library, whose instruments press
# up to 9 Notes per key. The RAM cost is ~+900 bytes per Synthesizer on ARM.
# Nobody has measured RP2040 or ESP32-P4 voice headroom under a full 64-voice
# load; the ceiling used to be an accidental CPU governor, and a board that
# cannot render 64 voices will now try and drop buffers rather than silently
# thin. Measure before shipping this to hardware.
#
# The original 8 -> 14 note follows. 8 was chosen as a
# modest middle ground before the instrument library existed to measure
# against. It does now, and 14 is not a preference but a fact about it:
# counted live, the resident permanent Note objects per drum kit are
# cr78 14, tr808 13, tr909 13, drumtraks 13, linndrum 13, sp1200 12,
# simmons_sdsv 12, dmx 11, tr707 11, tr606 8. At 8 -- and at 12 -- half the
# kits refuse notes, and by the paragraph above that refusal is silent. A
# ceiling below 14 is therefore a bug in this workspace, not a conservative
# choice; cr78 needs exactly 14.
#
# It also collapses the three-numbers problem: header, Make and CMake now
# all read 14, so a patch behaves the same wherever it is built, and the
# desktop value keeps matching the parity oracle's own build (14) as
# micropython.mk:116-122 requires.
#
# The cost is 20 bytes of GC heap per channel -- +120 bytes per Synthesizer
# against a 1024/2048-byte render buffer the same object already allocates
# unconditionally (src/synthio/__init__.c:327-329). Idle channels cost one
# pointer compare per block (:244-246). No gate anywhere exercises this
# constant: nothing in .github/ or tools/ invokes CMake.
#
# A board with less headroom can still override via CFLAGS_EXTRA or
# board-specific target_compile_definitions -- on CMake ports CFLAGS_EXTRA
# must be an ENVIRONMENT variable, which is how the shared infrastructure
# reads it (cmods/micropython/py/mkrules.cmake:79-86), not a make variable
# as on the unix Make path. Per-board tuning is the fuller phase 10 (port
# matrix) job, not this one.
# --- which audioif this firmware was built from -----------------------------
# See micropython.mk for why this is computed at build time and not stored.
# Quoted through CMake's generator so the strings survive as C string literals.
execute_process(
    COMMAND git -C ${MPAUDIO_MOD_DIR} describe --always --dirty --abbrev=7
    OUTPUT_VARIABLE MPAUDIO_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT MPAUDIO_REVISION)
    set(MPAUDIO_REVISION "unknown")
endif()
if(EXISTS ${MPAUDIO_MOD_DIR}/VERSION)
    file(READ ${MPAUDIO_MOD_DIR}/VERSION MPAUDIO_VERSION)
    string(STRIP "${MPAUDIO_VERSION}" MPAUDIO_VERSION)
else()
    set(MPAUDIO_VERSION "0.0.0+unknown")
endif()
target_compile_definitions(usermod_mpaudio INTERFACE
    AUDIOIF_VERSION=\"${MPAUDIO_VERSION}\"
    AUDIOIF_REVISION=\"${MPAUDIO_REVISION}\")

target_compile_definitions(usermod_mpaudio INTERFACE CIRCUITPY_SYNTHIO_MAX_CHANNELS=64)

target_link_libraries(usermod INTERFACE usermod_mpaudio)

# --- ulab (numpy-alike): a cloned dependency (see docs/porting-plan.md), not
#     part of this module, but `import ulab.numpy` in synthtools needs it in
#     the firmware. Mirrors micropython.mk:57-68 for standalone CMake
#     consumers: repo-local .deps/ (scripts/fetch_deps.sh, pinned by
#     DEPENDENCIES.lock) first, then the workspace sibling.
#
#     The two guard branches are load-bearing, not defensive noise -- do not
#     delete them as redundant. ulab's own code/micropython.cmake:1 calls
#     add_library(usermod_ulab INTERFACE) unguarded, so a second include of
#     it is a hard `another target with the same name already exists` error.
#     Under the workspace aggregator (cmods/micropython.cmake, which globs
#     mindepth 2/maxdepth 3) BOTH files are found and audioif's comes first
#     alphabetically -- so this block must stand down there and leave
#     cmods/ulab to the aggregator, exactly as before this block existed.
#     CMOD_DIR is how that is detected: cmods/micropython.cmake:1 sets it in
#     the same directory scope the included file runs in.
if(TARGET usermod_ulab)
    # Someone already supplied ulab (aggregator reached it before us).
    message(STATUS "audioif: ulab already provided by another usermod")
elseif(DEFINED CMOD_DIR AND EXISTS ${CMOD_DIR}/ulab/code/micropython.cmake)
    # Workspace aggregator build: it will include cmods/ulab itself.
    message(STATUS "audioif: ulab left to the workspace aggregator (${CMOD_DIR}/ulab)")
else()
    set(MPAUDIO_ULAB_CODE_DIR ${MPAUDIO_MOD_DIR}/.deps/ulab/code)
    if(NOT EXISTS ${MPAUDIO_ULAB_CODE_DIR}/ulab.c)
        set(MPAUDIO_ULAB_CODE_DIR ${MPAUDIO_MOD_DIR}/../ulab/code)
    endif()
    if(NOT EXISTS ${MPAUDIO_ULAB_CODE_DIR}/ulab.c AND NOT DEFINED ENV{AUDIOIF_OPTIONAL_DEPS})
        message(FATAL_ERROR "audioif: ulab not found (looked in .deps/ulab and ../ulab). Run ./scripts/fetch_deps.sh, or set AUDIOIF_OPTIONAL_DEPS=1 to build without it")
    endif()
    if(EXISTS ${MPAUDIO_ULAB_CODE_DIR}/ulab.c)
        message(STATUS "audioif: ulab from ${MPAUDIO_ULAB_CODE_DIR}")
        include(${MPAUDIO_ULAB_CODE_DIR}/micropython.cmake)
    endif()
endif()
