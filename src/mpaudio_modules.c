// Module skeleton for audiodsp (tier 0 / phase 2 of
// docs/porting-plan.md): registers the top-level modules the port will
// grow into, each currently empty (just __name__). Each tier fills in its
// module's globals table directly in this file as ported classes/functions
// land -- so `import <name>` succeeds from the start of a tier's work, and
// this file's history is the port's tier-by-tier progress.
//
// This is new code (not a CircuitPython port): CircuitPython doesn't need
// an explicit module-registration file since its module tables are wired
// through its own build-time manifest system instead of MP_REGISTER_MODULE.

#include "py/obj.h"
#include "py/runtime.h"

// audiocore (tier 1) has real content now -- see audiocore/module.c.
// synthio (tier 2) has real content now -- see synthio/__init__.c.
// audiomixer (tier 3) has real content now -- see audiomixer/module.c.

// audiofilters (tier 4) has real content now -- see audiofilters/module.c.

// audiodelays (tier 4) has real content now -- see audiodelays/module.c.

// audiofreeverb (tier 4) has real content now -- see audiofreeverb/module.c.

// audiospeed (tier 4) has real content now -- see audiospeed/module.c.

// audiomp3 (tier 5) has real content now -- see audiomp3/module.c.

// audioladder (tier 6) has real content now -- see audioladder/module.c.

// audiodynamics / audioroute (tier 6) are the first modules here that are not
// CircuitPython ports at all -- they come from micropython-vst3's `vstaudio`
// usermod. See audiodynamics/module.c, audioroute/module.c.

// audiomath, audioecho, audioshaper, audioconvolve, audiobiquad and
// audioverb and audiomodal (tier 6) are audiodsp's own, with no ancestor
// anywhere.
// See each module.c.
