"""No Tremor symbol escapes audiodsp's namespace.

    vorbis_symbols_check.py            # PASS, or FAIL and exit 1
    vorbis_symbols_check.py --fault    # plants an unrenamed symbol: must FAIL

A firmware can link a second copy of Tremor beside audiodsp's (a streaming
client that brought its own), and two definitions of ov_read don't link.
src/shared/audiodsp_tremor_names.h renames every external symbol Tremor
defines, so this compiles src/shared/audiodsp_vorbis.c on its own and reads
the object's global symbols: each must start with audiodsp_. A Tremor update
that adds a function fails here, with its name, instead of at some board's
link step.

Needs a C compiler (cc, or $CC) and nm.
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def main():
    fault = "--fault" in sys.argv
    cc = os.environ.get("CC", "cc")
    if shutil.which(cc) is None or shutil.which("nm") is None:
        sys.exit("vorbis_symbols_check needs a C compiler and nm")
    with tempfile.TemporaryDirectory() as tmp:
        obj = Path(tmp) / "audiodsp_vorbis.o"
        command = [cc, "-c", "-O1", "-I", str(ROOT / "src"),
                   str(ROOT / "src" / "shared" / "audiodsp_vorbis.c"), "-o", str(obj)]
        if fault:
            planted = Path(tmp) / "planted.h"
            planted.write_text("int ov_planted_symbol(void);\n"
                               "int ov_planted_symbol(void) { return 0; }\n")
            command[1:1] = ["-include", str(planted)]
        subprocess.run(command, check=True)
        listing = subprocess.run(["nm", "-g", "--defined-only", str(obj)],
                                 capture_output=True, text=True, check=True).stdout
    names = [line.split()[-1] for line in listing.splitlines() if line.strip()]
    # Mach-O prefixes every C symbol with an underscore.
    names = [n[1:] if sys.platform == "darwin" and n.startswith("_") else n for n in names]
    escaped = sorted(n for n in names if not n.startswith("audiodsp_"))
    print("%d global symbols, %d outside audiodsp_" % (len(names), len(escaped)))
    for name in escaped:
        print("  escaped:", name)
    if escaped or not names:
        print("VORBIS SYMBOLS FAIL")
        return 1
    print("VORBIS SYMBOLS PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
