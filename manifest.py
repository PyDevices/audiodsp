"""audiodsp's freeze manifest: deliberately empty.

The parent workspace's freeze manifests (one per interpreter) include this
file for every build they make, so anything unconditional here would land in
every interpreter in the workspace. There is nothing to put here:

- The native modules (``audiocore``, ``synthio``, ``audiodynamics``,
  ``audioroute``, ``audiomath``, ``audioecho``, ``audioshaper``,
  ``audioladder``, ``audioconvolve``, ``audiobiquad``, ``audioverb``,
  ``audiomodal``
  and the rest) are compiled into the firmware by
  ``micropython.mk`` /
  ``micropython.cmake`` (or, for CircuitPython, by ``apply_cp_patches.sh``).
  A manifest never sees them.
- ``lib/audiorender`` renders a whole composition offline with numpy and holds
  the finished song in memory, which is a desktop's job, not a board's. It
  ships in the wheel and stops there.
- The instrument and effect libraries (``audioinstruments``,
  ``audioeffects``) no longer live in this repository. They are developed
  and published from https://github.com/PyDevices/audiocomponents and reach
  boards by MIP from there. The opt-in ``AUDIODSP_FREEZE_LIBS`` switch that
  used to freeze this repository's copies went with them; a frozen copy
  would shadow the mip-installed one, and what is running would stop being
  what was published - the trap ``pydevices``' own manifest refuses to walk
  into.
"""

# MicroPython 1.29: the manifest names its own C module (workspace retool, piece 1).
c_module(".")  # this directory holds the micropython.cmake / micropython.mk for the C half
