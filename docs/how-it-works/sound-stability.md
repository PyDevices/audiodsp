# Sound stability

What stays the same from one release to the next, for audiodsp and for the
instruments and effects built on it in
[audioinstruments](https://github.com/PyDevices/audioinstruments) and
[audioeffects](https://github.com/PyDevices/audioeffects).

**The API is stable.** Class names, signatures, metadata and macro surfaces
change only deliberately, and the changelog says when.

**An instrument's or an effect's sound is not.** They sound good, but they
aren't all as accurate as they could be, and they keep being refined, so one
may sound audibly different in the next release. If a piece depends on the
exact sound of a release, pin that release rather than tracking the latest.
Every release stays installable for exactly this reason.

**The floor under them doesn't move.** audiodsp's CircuitPython-compatible
modules (`synthio`, `audiocore` and the effects modules) render the same bytes
as CircuitPython itself, release after release. Where the two disagree, it's a
bug, and it's reported upstream. The few places audiodsp differs on purpose are
listed in [Where audiodsp differs from CircuitPython](upstream-diff.md).
