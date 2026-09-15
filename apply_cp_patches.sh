#!/usr/bin/env bash
# Add audioif's audiodynamics, audioroute (Splitter, MidSide),
# audiomath, audioecho, audioshaper, audioladder, audioconvolve,
# audiobiquad, audioverb and audiomodal modules to a CircuitPython tree.
#
#   ./apply_cp_patches.sh --dry-run [--port PORT] [--variant VARIANT]
#   ./apply_cp_patches.sh --apply   [--port PORT] [--variant VARIANT]
#   ./apply_cp_patches.sh --status  [--port PORT] [--variant VARIANT]
#
# CircuitPython already has everything else audioif ports - audiocore, synthio,
# audiomixer, the effects - so this script only *adds*: modules that
# CircuitPython never had, whose DSP is the same src/shared/ C the MicroPython
# usermod and the CPython extension compile. The one exception is a rewrite of
# audiocore.get_buffer's return type; see src/circuitpython_spike/
# apply_replacements.py for what and why.
#
# Out-of-tree substitute for Adafruit's Extending CircuitPython (no upstream
# PR), same shape as displayif/apply_cp_patches.sh:
#   Learn / design-guide step        This script
#   shared-bindings/<mod>/           copy spike -> CP shared-bindings/
#   shared-module/<mod>/             copy spike -> CP shared-module/
#   enable CIRCUITPY_*               variant .mk + py/circuitpy_mpconfig.mk
#   list sources in the port build   SRC_PATTERNS + the variant's own list
#
# Environment:
#   CP_DIR          CircuitPython tree (default: sibling circuitpython/)
#   WORKSPACE_DIR   Parent of audioif (default: parent of this repo)
#   PORT            Must be unix (default: unix); other ports skip with exit 0
#   VARIANT         Unix variant (default: coverage)
#
# The unix `coverage` variant is the workspace's parity oracle, and it hand-
# lists every source it builds (SRC_BITMAP) rather than going through
# SRC_PATTERNS - so both have to be written, and the hand-list is the one that
# actually decides whether the module compiles here.

set -euo pipefail

AUDIOIF_DIR=$(cd "$(dirname "$0")" && pwd)
WORKSPACE_DIR="${WORKSPACE_DIR:-$(cd "$AUDIOIF_DIR/.." && pwd)}"
SPIKE_DIR="$AUDIOIF_DIR/src/circuitpython_spike"
MANIFEST="$SPIKE_DIR/copy_manifest.txt"
REPLACEMENTS="$SPIKE_DIR/apply_replacements.py"

PORT="${PORT:-unix}"
VARIANT="${VARIANT:-coverage}"
MODE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run|--apply|--status) MODE="$1"; shift ;;
        --port)    PORT="$2"; shift 2 ;;
        --variant) VARIANT="$2"; shift 2 ;;
        -h|--help) sed -n '2,31p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1 (try --help)" >&2; exit 1 ;;
    esac
done
MODE="${MODE:---dry-run}"

if [[ "$PORT" != unix ]]; then
    echo "audioif apply_cp_patches: port=$PORT is not unix; skipping"
    exit 0
fi

if [[ -n "${CP_DIR:-}" && -d "${CP_DIR}/ports" ]]; then
    CP_DIR=$(cd "$CP_DIR" && pwd)
elif [[ -d "$WORKSPACE_DIR/circuitpython/ports" ]]; then
    CP_DIR=$(cd "$WORKSPACE_DIR/circuitpython" && pwd)
else
    echo "CircuitPython tree not found (set CP_DIR)." >&2
    exit 1
fi

# Oracle pin check (finding 10): warn loudly when the tree is not the
# declared oracle commit -- a moved pin silently invalidates every golden.
ORACLE_FILE="$AUDIOIF_DIR/CIRCUITPYTHON_ORACLE"
if [[ -f "$ORACLE_FILE" ]]; then
    ORACLE_SHA=$(awk '/^[^#]/ {print $2; exit}' "$ORACLE_FILE")
    ACTUAL_SHA=$(git -C "$CP_DIR" rev-parse HEAD 2>/dev/null || echo unknown)
    if [[ "$ACTUAL_SHA" != "$ORACLE_SHA" ]]; then
        echo "WARNING: CircuitPython tree at $CP_DIR is $ACTUAL_SHA," >&2
        echo "         but CIRCUITPYTHON_ORACLE declares $ORACLE_SHA (10.2.1)." >&2
        echo "         Parity goldens are only meaningful against the pin." >&2
    fi
fi

PORT_DIR="$CP_DIR/ports/$PORT"
VARIANT_MK="$PORT_DIR/variants/$VARIANT/mpconfigvariant.mk"
VARIANT_H="$PORT_DIR/variants/$VARIANT/mpconfigvariant.h"
DEFNS_MK="$CP_DIR/py/circuitpy_defns.mk"
MPCONFIG_MK="$CP_DIR/py/circuitpy_mpconfig.mk"

MARKER_TAG="audioif-cp begin (apply_cp_patches.sh)"

DRY_RUN=0
[[ "$MODE" == "--dry-run" ]] && DRY_RUN=1
# Nonzero if any dry-run check finds something --apply could not do.
DRY_RC=0

markers_for_file() {
    case "$1" in
        *.h) echo "/* >>> $MARKER_TAG */"; echo "/* >>> audioif-cp end */" ;;
        *)   echo "# >>> $MARKER_TAG";     echo "# >>> audioif-cp end" ;;
    esac
}

block_present() {
    [ -f "$1" ] && grep -qF "${2:-audioif-cp begin}" "$1"
}

# Insert a marked block after the first line containing `anchor` -- or, when
# the markers are already there, rewrite what is between them.
#
# That second half matters: adding a module to this script has to reach a tree
# that was patched by an earlier version of it. Skipping on "marker present"
# used to mean a new module's CIRCUITPY_* flag silently never landed, and the
# build then failed a long way from the cause.
insert_block_after() {
    local file="$1" anchor="$2" block="$3" needle="${4:-audioif-cp begin}"
    local begin end
    begin=$(markers_for_file "$file" | sed -n '1p')
    end=$(markers_for_file "$file" | sed -n '2p')
    if [ "$DRY_RUN" = 1 ]; then
        if block_present "$file" "$needle"; then
            echo "  [dry-run] refresh block in ${file#"$CP_DIR"/}"
        elif grep -qF "$anchor" "$file" 2>/dev/null; then
            echo "  [dry-run] insert into ${file#"$CP_DIR"/} after: $anchor"
        else
            # The apply path raises "anchor not found" here. The dry run used
            # to print the same line either way, so a moved upstream anchor -
            # the one thing a pin move must stop on - was invisible until the
            # apply.
            echo "  [dry-run] ERROR: anchor not found in ${file#"$CP_DIR"/}: $anchor"
            DRY_RC=1
        fi
        return 0
    fi
    local outcome
    outcome=$(python3 - "$file" "$anchor" "$begin" "$end" "$block" <<'PY'
import sys
from pathlib import Path

path, anchor, begin, end, block = sys.argv[1:6]
text = Path(path).read_text()
if begin in text:
    head, _, rest = text.partition(begin)
    body, separator, tail = rest.partition(end)
    if not separator:
        raise SystemExit("unterminated block in %s" % path)
    if body.strip("\n") == block:
        print("current")
        raise SystemExit(0)
    Path(path).write_text(head + begin + "\n" + block + "\n" + end + tail)
    print("updated")
    raise SystemExit(0)
if anchor not in text:
    raise SystemExit("anchor not found in %s: %r" % (path, anchor))
Path(path).write_text(
    text.replace(anchor, anchor + "\n%s\n%s\n%s" % (begin, block, end), 1))
print("patched")
PY
    )
    echo "  ${outcome}: ${file#"$CP_DIR"/}"
}

# Insert one plain line after `anchor` - for the build lists, where a marker
# comment inside a backslash-continued variable would break the continuation.
insert_line_after() {
    local file="$1" anchor="$2" line="$3"
    if grep -qF "$line" "$file" 2>/dev/null; then
        echo "  skip (already present): ${file#"$CP_DIR"/}"
        return 0
    fi
    if [ "$DRY_RUN" = 1 ]; then
        if grep -qF "$anchor" "$file" 2>/dev/null; then
            echo "  [dry-run] insert into ${file#"$CP_DIR"/}: ${line//$'\t'/}"
        else
            echo "  [dry-run] ERROR: anchor not found in ${file#"$CP_DIR"/}: $anchor"
            DRY_RC=1
        fi
        return 0
    fi
    python3 - "$file" "$anchor" "$line" <<'PY'
import sys
from pathlib import Path

path, anchor, line = sys.argv[1:4]
text = Path(path).read_text()
if line in text:
    raise SystemExit(0)
if anchor not in text:
    raise SystemExit("anchor not found in %s: %r" % (path, anchor))
Path(path).write_text(text.replace(anchor, anchor + "\n" + line, 1))
PY
    echo "  added:   ${line//$'\t'/} -> ${file#"$CP_DIR"/}"
}

copy_files() {
    python3 - "$AUDIOIF_DIR" "$CP_DIR" "$MANIFEST" "$DRY_RUN" <<'PY'
import filecmp
import shutil
import sys
from pathlib import Path

source_root, cp_dir, manifest, dry = sys.argv[1:5]
dry_run = dry == "1"
for raw in Path(manifest).read_text().splitlines():
    line = raw.split("#", 1)[0].strip()
    if not line:
        continue
    source_rel, destination_rel = (part.strip() for part in line.split("\t", 1))
    source = Path(source_root) / source_rel
    destination = Path(cp_dir) / destination_rel
    if not source.is_file():
        raise SystemExit("missing source file: %s" % source)
    if destination.is_file() and filecmp.cmp(source, destination, shallow=False):
        print("  unchanged: %s" % destination_rel)
        continue
    if dry_run:
        print("  [dry-run] %s %s"
              % ("update" if destination.is_file() else "create",
                 destination_rel))
        continue
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    print("  copied:    %s" % destination_rel)
PY
}

# Anchors chain off whatever sibling patch sets are already in the file, so
# repeated runs of several repos' scripts stack instead of fighting.
last_marker_or() {
    local file="$1"; shift
    local candidate
    for candidate in "$@"; do
        if grep -qF "$candidate" "$file"; then
            echo "$candidate"
            return 0
        fi
    done
    echo "no usable anchor in $file (tried: $*)" >&2
    exit 1
}

echo "CircuitPython: $CP_DIR"
echo "audioif:       $AUDIOIF_DIR"
echo "variant:       $VARIANT"
echo "mode:          $MODE"
echo

if [[ "$MODE" == "--status" ]]; then
    # Anything not in the state --apply would leave behind makes --status exit
    # nonzero. It used to report the defect and exit 0, so a caller that
    # checked $? read a broken tree as a good one - found by planting a fault
    # in the pin move's step-0 rehearsal, 2026-09-09.
    STATUS_RC=0
    for file in "$VARIANT_MK" "$VARIANT_H" "$DEFNS_MK" "$MPCONFIG_MK"; do
        if [ ! -e "$file" ]; then
            echo "missing  ${file#"$CP_DIR"/}"; STATUS_RC=1
        elif block_present "$file"; then
            echo "patched  ${file#"$CP_DIR"/}"
        else
            echo "pending  ${file#"$CP_DIR"/}"; STATUS_RC=1
        fi
    done
    for file in shared-bindings/audiodynamics/__init__.c \
                shared-bindings/audioroute/__init__.c \
                shared-bindings/audiomath/__init__.c \
                shared-bindings/audioecho/__init__.c \
                shared-bindings/audioshaper/__init__.c \
                shared-bindings/audioladder/__init__.c \
                shared-bindings/audioconvolve/__init__.c \
                shared-bindings/audiobiquad/__init__.c \
                shared-bindings/audioverb/__init__.c \
                shared-bindings/audiomodal/__init__.c \
                shared/audioif_dynamics.c shared/audioif_splitter.c \
                shared/audioif_midside.c \
                shared/audioif_multiply.c shared/audioif_suboctave.c \
                shared/audioif_feedback_delay.c \
                shared/audioif_shaper.c \
                shared/audioif_ladder.c \
                shared/audioif_modal.c \
                shared/audioif_trig.c shared/audioif_fft.c \
                shared/audioif_convolve.c shared/audioif_filter_f32.c \
                shared/audioif_tank.c; do
        if [ -e "$CP_DIR/$file" ]; then
            echo "ok       $file"
        else
            echo "missing  $file"; STATUS_RC=1
        fi
    done
    python3 "$REPLACEMENTS" "$CP_DIR" status || STATUS_RC=1
    exit $STATUS_RC
fi

echo "==> Copy modules and shared DSP"
copy_files
echo

echo "==> Rewrite audiocore.get_buffer's return type"
python3 "$REPLACEMENTS" "$CP_DIR" "${MODE#--}"
echo

echo "==> py/circuitpy_mpconfig.mk (off by default)"
MPCONFIG_ANCHOR=$(last_marker_or "$MPCONFIG_MK" \
    '# >>> lv-circuitpython-mod end' \
    'CFLAGS += -DCIRCUITPY_LOCALE=$(CIRCUITPY_LOCALE)')
insert_block_after "$MPCONFIG_MK" "$MPCONFIG_ANCHOR" \
"CIRCUITPY_AUDIODYNAMICS ?= 0
CFLAGS += -DCIRCUITPY_AUDIODYNAMICS=\$(CIRCUITPY_AUDIODYNAMICS)
CIRCUITPY_AUDIOROUTE ?= 0
CFLAGS += -DCIRCUITPY_AUDIOROUTE=\$(CIRCUITPY_AUDIOROUTE)
CIRCUITPY_AUDIOMATH ?= 0
CFLAGS += -DCIRCUITPY_AUDIOMATH=\$(CIRCUITPY_AUDIOMATH)
CIRCUITPY_AUDIOECHO ?= 0
CFLAGS += -DCIRCUITPY_AUDIOECHO=\$(CIRCUITPY_AUDIOECHO)
CIRCUITPY_AUDIOSHAPER ?= 0
CFLAGS += -DCIRCUITPY_AUDIOSHAPER=\$(CIRCUITPY_AUDIOSHAPER)
CIRCUITPY_AUDIOLADDER ?= 0
CFLAGS += -DCIRCUITPY_AUDIOLADDER=\$(CIRCUITPY_AUDIOLADDER)
CIRCUITPY_AUDIOCONVOLVE ?= 0
CFLAGS += -DCIRCUITPY_AUDIOCONVOLVE=\$(CIRCUITPY_AUDIOCONVOLVE)
CIRCUITPY_AUDIOBIQUAD ?= 0
CFLAGS += -DCIRCUITPY_AUDIOBIQUAD=\$(CIRCUITPY_AUDIOBIQUAD)
CIRCUITPY_AUDIOVERB ?= 0
CFLAGS += -DCIRCUITPY_AUDIOVERB=\$(CIRCUITPY_AUDIOVERB)
CIRCUITPY_AUDIOMODAL ?= 0
CFLAGS += -DCIRCUITPY_AUDIOMODAL=\$(CIRCUITPY_AUDIOMODAL)"
echo

echo "==> py/circuitpy_defns.mk (source patterns)"
DEFNS_ANCHOR=$(last_marker_or "$DEFNS_MK" \
    '# >>> lv-circuitpython-mod end' \
    'ifeq ($(CIRCUITPY_MATH),1)')
insert_block_after "$DEFNS_MK" "$DEFNS_ANCHOR" \
"ifeq (\$(CIRCUITPY_AUDIODYNAMICS),1)
SRC_PATTERNS += audiodynamics/%
endif
ifeq (\$(CIRCUITPY_AUDIOROUTE),1)
SRC_PATTERNS += audioroute/%
endif
ifeq (\$(CIRCUITPY_AUDIOMATH),1)
SRC_PATTERNS += audiomath/%
endif
ifeq (\$(CIRCUITPY_AUDIOECHO),1)
SRC_PATTERNS += audioecho/%
endif
ifeq (\$(CIRCUITPY_AUDIOSHAPER),1)
SRC_PATTERNS += audioshaper/%
endif
ifeq (\$(CIRCUITPY_AUDIOLADDER),1)
SRC_PATTERNS += audioladder/%
endif
ifeq (\$(CIRCUITPY_AUDIOCONVOLVE),1)
SRC_PATTERNS += audioconvolve/%
endif
ifeq (\$(CIRCUITPY_AUDIOBIQUAD),1)
SRC_PATTERNS += audiobiquad/%
endif
ifeq (\$(CIRCUITPY_AUDIOVERB),1)
SRC_PATTERNS += audioverb/%
endif
ifeq (\$(CIRCUITPY_AUDIOMODAL),1)
SRC_PATTERNS += audiomodal/%
endif" "SRC_PATTERNS += audiodynamics/%"
echo

echo "==> Unix variant: enable the modules"
[ -f "$VARIANT_MK" ] || { echo "Variant makefile not found: $VARIANT_MK" >&2; exit 1; }
VARIANT_ANCHOR=$(last_marker_or "$VARIANT_MK" \
    '# >>> displayif-usdl2 end' \
    '# >>> lv-circuitpython-mod end' \
    'CIRCUITPY_MESSAGE_COMPRESSION_LEVEL = 1')
insert_block_after "$VARIANT_MK" "$VARIANT_ANCHOR" \
"CIRCUITPY_AUDIODYNAMICS = 1
CFLAGS += -DCIRCUITPY_AUDIODYNAMICS=1
CIRCUITPY_AUDIOROUTE = 1
CFLAGS += -DCIRCUITPY_AUDIOROUTE=1
CIRCUITPY_AUDIOMATH = 1
CFLAGS += -DCIRCUITPY_AUDIOMATH=1
CIRCUITPY_AUDIOECHO = 1
CFLAGS += -DCIRCUITPY_AUDIOECHO=1
CIRCUITPY_AUDIOSHAPER = 1
CFLAGS += -DCIRCUITPY_AUDIOSHAPER=1
CIRCUITPY_AUDIOLADDER = 1
CFLAGS += -DCIRCUITPY_AUDIOLADDER=1
CIRCUITPY_AUDIOCONVOLVE = 1
CFLAGS += -DCIRCUITPY_AUDIOCONVOLVE=1
CIRCUITPY_AUDIOBIQUAD = 1
CFLAGS += -DCIRCUITPY_AUDIOBIQUAD=1
CIRCUITPY_AUDIOVERB = 1
CFLAGS += -DCIRCUITPY_AUDIOVERB=1
CIRCUITPY_AUDIOMODAL = 1
CFLAGS += -DCIRCUITPY_AUDIOMODAL=1
CIRCUITPY_AUDIOSPEED = 1
CFLAGS += -DCIRCUITPY_AUDIOSPEED=1

# audiospeed's rate validator is called with fractional bounds -
# mp_arg_validate_obj_float_range(rate_obj, 0.001, 1000.0, ...) - whose
# parameters are mp_int_t, so 0.001 truncates to 0. ports/unix/Makefile is the
# only port compiling with -Werror -Wfloat-conversion and raspberrypi is the
# only port enabling audiospeed, so upstream never builds this combination.
# Their unix port is unsupported, so this is ours to absorb, not a PR. Downgrade
# the warning for these objects ONLY, never for the build: audioif's own float
# cell keeps its -Werror (audioif#4).
\$(BUILD)/shared-module/audiospeed/__init__.o: CFLAGS += -Wno-error=float-conversion
\$(BUILD)/shared-bindings/audiospeed/__init__.o: CFLAGS += -Wno-error=float-conversion"
echo

echo "==> Unix variant: source list"
# The coverage variant hand-lists its sources and never consults SRC_PATTERNS,
# so these lines are what actually gets the modules compiled here.
BINDING_ANCHOR=$'\tshared-bindings/audiofilters/__init__.c \\'
MODULE_ANCHOR=$'\tshared-module/audiofilters/__init__.c \\'
# audiospeed is an UPSTREAM module, enabled here for the effects programme's
# stock resampler/speed-changer. Its default in py/circuitpy_mpconfig.mk stays
# 0, so no board gains it by accident, and its -D flag comes from upstream
# CFLAGS, so it needs no mpconfigvariant.h guard the way our own modules do.
# It needs the warning downgrade in the enable block above; see the note there.
#
# audiofilewriter is deliberately NOT enabled: it compiles but does not LINK on
# this variant, calling background_callback_prevent/allow while coverage builds
# no supervisor/ at all. Supplying them pulls supervisor/port.h,
# supervisor/shared/tick.h and shared-bindings/microcontroller into a test
# build - a port change, not a source-list line. Established by building,
# 2026-09-09; see the pin-move evidence file.
for _f in shared-bindings/audiospeed/__init__.c \
          shared-bindings/audiospeed/Resampler.c \
          shared-bindings/audiospeed/SpeedChanger.c; do
    insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\t'"$_f"$' \\'
done
for _f in shared-module/audiospeed/__init__.c \
          shared-module/audiospeed/Resampler.c \
          shared-module/audiospeed/SpeedChanger.c; do
    insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\t'"$_f"$' \\'
done
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audiodynamics/Dynamics.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audiodynamics/__init__.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioroute/MidSide.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioroute/Splitter.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioroute/SplitterTap.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioroute/__init__.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audiomath/Multiply.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audiomath/SubOctave.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audiomath/__init__.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioecho/FeedbackDelay.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioecho/__init__.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioshaper/Waveshaper.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioshaper/__init__.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioladder/Ladder.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioladder/__init__.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioconvolve/Convolver.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioconvolve/__init__.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audiobiquad/Biquad.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audiobiquad/AllPass.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audiobiquad/__init__.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioverb/Tank.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audioverb/__init__.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audiomodal/Bank.c \\'
insert_line_after "$VARIANT_MK" "$BINDING_ANCHOR" $'\tshared-bindings/audiomodal/__init__.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audiodynamics/Dynamics.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audioroute/MidSide.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audioroute/Splitter.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audioroute/SplitterTap.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audiomath/Multiply.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audiomath/SubOctave.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audioecho/FeedbackDelay.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audioshaper/Waveshaper.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audioladder/Ladder.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audioconvolve/Convolver.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audiobiquad/Biquad.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audiobiquad/AllPass.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audioverb/Tank.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared-module/audiomodal/Bank.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_modal.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_dynamics.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_splitter.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_multiply.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_suboctave.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_midside.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_feedback_delay.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_shaper.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_ladder.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_trig.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_fft.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_convolve.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_filter_f32.c \\'
insert_line_after "$VARIANT_MK" "$MODULE_ANCHOR" $'\tshared/audioif_tank.c \\'
echo

echo "==> Unix variant: mpconfigvariant.h guards"
if [ -f "$VARIANT_H" ]; then
    VARIANT_H_ANCHOR=$(last_marker_or "$VARIANT_H" \
        '/* >>> displayif-usdl2 end */' \
        '/* >>> lv-circuitpython-mod end */' \
        '#include "../mpconfigvariant_common.h"')
    insert_block_after "$VARIANT_H" "$VARIANT_H_ANCHOR" \
"#ifndef CIRCUITPY_AUDIODYNAMICS
#define CIRCUITPY_AUDIODYNAMICS (0)
#endif
#ifndef CIRCUITPY_AUDIOROUTE
#define CIRCUITPY_AUDIOROUTE (0)
#endif
#ifndef CIRCUITPY_AUDIOMATH
#define CIRCUITPY_AUDIOMATH (0)
#endif
#ifndef CIRCUITPY_AUDIOECHO
#define CIRCUITPY_AUDIOECHO (0)
#endif
#ifndef CIRCUITPY_AUDIOSHAPER
#define CIRCUITPY_AUDIOSHAPER (0)
#endif
#ifndef CIRCUITPY_AUDIOLADDER
#define CIRCUITPY_AUDIOLADDER (0)
#endif
#ifndef CIRCUITPY_AUDIOCONVOLVE
#define CIRCUITPY_AUDIOCONVOLVE (0)
#endif
#ifndef CIRCUITPY_AUDIOBIQUAD
#define CIRCUITPY_AUDIOBIQUAD (0)
#endif
#ifndef CIRCUITPY_AUDIOMODAL
#define CIRCUITPY_AUDIOMODAL (0)
#endif
#ifndef CIRCUITPY_AUDIOVERB
#define CIRCUITPY_AUDIOVERB (0)
#endif"
fi
echo

if [ "$DRY_RUN" = 1 ]; then
    echo "Dry run complete. Re-run with --apply to write changes."
    [ "$DRY_RC" = 0 ] || echo "Dry run reported at least one ERROR above."
    exit "$DRY_RC"
else
    echo "Patches applied."
    echo
    echo "Next:"
    echo "  cd $PORT_DIR && make -j VARIANT=$VARIANT"
    echo "  or, in this workspace: run the parent workspace's own build-interpreters script"
fi
