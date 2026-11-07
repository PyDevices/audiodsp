# A test driver for the pump's gates: a sink that takes a few bytes at a time,
# as a board's DAC with no thread does, and keeps what it took for a probe to
# read (tests/pump/pump_probe.py, case `tail`). Build it beside audiodsp, as a
# second user C module, for a unix binary that runs the gates; never in
# firmware. Its sink stays closed unless a probe opens it, so the other cases
# run as they would with no driver.

SINKSTUB_DIR := $(USERMOD_DIR)
SRC_USERMOD_C += $(SINKSTUB_DIR)/sinkstub.c
# audiodsp's own headers (shared/audiodsp_port.h), three levels up, whichever
# order the build reads the two modules in.
CFLAGS_USERMOD += -I$(SINKSTUB_DIR)/../../../src
