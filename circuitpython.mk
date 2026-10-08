# CircuitPython build glue for audiodsp, read through USER_C_MODULES.
#
# micropython.mk includes this file when the tree being built is CircuitPython,
# so one module directory serves both interpreters:
#
#   make -C ports/unix USER_C_MODULES=/path/to/audiodsp
#   make -C ports/espressif BOARD=... USER_C_MODULES=/path/to/audiodsp
#
# CircuitPython already has audiocore, synthio, audiomixer, audiomp3,
# audiofilters, audiodelays, audiofreeverb and audiospeed, and its audio
# outputs (audiobusio, audioio, audiopwmio) play anything that speaks its
# audiosample protocol. So this compiles only the modules CircuitPython lacks,
# written against that protocol (src/circuitpython_spike/), plus the
# runtime-neutral DSP they share with the MicroPython and CPython builds
# (src/shared/). CircuitPython's own audio modules stay CircuitPython's: a
# second audiocore under the same name would not link, and boards' audio
# outputs are built against theirs.
#
# Each binding registers its module with MP_REGISTER_MODULE, so nothing in
# CircuitPython's own tree is edited and no CIRCUITPY_* switch is needed for
# these modules. CircuitPython's audiocore is needed (CIRCUITPY_AUDIOCORE=1,
# the default wherever a board has an audio output, and on the unix port).

AUDIODSP_CP_DIR := $(USERMOD_DIR)
AUDIODSP_CP_SRC := $(AUDIODSP_CP_DIR)/src
AUDIODSP_CP_SPIKE := $(AUDIODSP_CP_SRC)/circuitpython_spike

# The bindings include "shared-bindings/<module>/..." and "shared-module/...",
# and the DSP "shared/audiodsp_*.h". CircuitPython's own -I$(TOP) comes first,
# so its audiocore and synthio headers are the ones these resolve to.
CFLAGS_USERMOD += -I$(AUDIODSP_CP_SPIKE) -I$(AUDIODSP_CP_SRC)

AUDIODSP_CP_MODULES := audiodynamics audioroute audiomath audioecho audioshaper \
	audioladder audioconvolve audiobiquad audioverb audiomodal

AUDIODSP_CP_SOURCES := \
	$(foreach m,$(AUDIODSP_CP_MODULES),$(sort $(wildcard $(AUDIODSP_CP_SPIKE)/shared-bindings/$(m)/*.c))) \
	$(foreach m,$(AUDIODSP_CP_MODULES),$(sort $(wildcard $(AUDIODSP_CP_SPIKE)/shared-module/$(m)/*.c)))

AUDIODSP_CP_SOURCES += $(addprefix $(AUDIODSP_CP_SRC)/shared/, \
	audiodsp_dynamics.c \
	audiodsp_splitter.c \
	audiodsp_midside.c \
	audiodsp_multiply.c \
	audiodsp_suboctave.c \
	audiodsp_feedback_delay.c \
	audiodsp_shaper.c \
	audiodsp_samplehold.c \
	audiodsp_ladder.c \
	audiodsp_trig.c \
	audiodsp_fft.c \
	audiodsp_convolve.c \
	audiodsp_filter_f32.c \
	audiodsp_tank.c \
	audiodsp_modal.c \
	)

SRC_USERMOD_C += $(AUDIODSP_CP_SOURCES)

# CircuitPython's board ports append -Werror=float-equal and
# -Werror=double-promotion (espressif, among others) after CFLAGS_USERMOD, so a
# module-wide -Wno-* would lose. The shared float kernels compare against exact
# zero on purpose (a tail that reaches exact zero is the point of several of
# them) and are byte-for-byte the code the MicroPython and CPython builds run,
# so they are built as they are, with the two warnings off for their objects
# only. py.mk puts a module's objects under $(BUILD)/<module directory name>/.
AUDIODSP_CP_OBJ_CFLAGS := -Wno-float-equal -Wno-double-promotion
$(foreach _s,$(filter $(AUDIODSP_CP_SRC)/shared/%,$(AUDIODSP_CP_SOURCES)),\
    $(eval $(BUILD)/$(notdir $(AUDIODSP_CP_DIR))/$(patsubst $(AUDIODSP_CP_DIR)/%.c,%.o,$(_s)): CFLAGS += $(AUDIODSP_CP_OBJ_CFLAGS)))
