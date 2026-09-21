#!/usr/bin/env python3
"""Rewrites inside CircuitPython's own sources that apply_cp_patches.sh makes.

    apply_replacements.py <circuitpython dir> apply|dry-run|status

Everything else that script does is additive - new files, new lines in a build
list - and marker comments are enough to make those idempotent. What lives here
is the one place audiodsp has to change code CircuitPython already had, which
needs a before as well as an after.

Each rewrite is idempotent, and fails loudly rather than quietly when neither
the marker nor the original text is present: that means the file moved
underneath us upstream and a person should look at it.
"""

import sys
from pathlib import Path

MARKER = "/* >>> audiodsp-cp begin (apply_cp_patches.sh) */"

#: (path in the CircuitPython tree, the text as upstream writes it, ours)
#:
#: audiocore.get_buffer handed back a memoryview typed by the sample's width,
#: so len() counted samples while the C protocol's buffer_length counts bytes.
#: Every byte calculation downstream was then wrong by the sample width - a
#: silent 2x for ordinary 16-bit audio. audiodsp's own audiocore returns a byte
#: view; this makes the oracle agree, so parity captures compare like with
#: like. See audiodsp 413d87a.
REPLACEMENTS = [
    (
        "shared-bindings/audiocore/__init__.c",
        """    // audiosample_get_buffer checked that we're a sample so this is a safe cast
    audiosample_base_t *sample = MP_OBJ_TO_PTR(sample_in);

    mp_obj_t result[2] = {mp_obj_new_int_from_uint(gbr), mp_const_none};

    if (gbr != GET_BUFFER_ERROR) {
        bool single_buffer, samples_signed;
        uint32_t max_buffer_length;
        uint8_t spacing;

        uint8_t bits_per_sample = audiosample_get_bits_per_sample(sample);
        audiosample_get_buffer_structure(sample, false, &single_buffer, &samples_signed, &max_buffer_length, &spacing);
        // copies the data because the gc semantics of get_buffer are unclear
        void *result_buf = m_malloc_without_collect(buffer_length);
        memcpy(result_buf, buffer, buffer_length);
        char typecode =
            (bits_per_sample == 8 && samples_signed) ? 'b' :
            (bits_per_sample == 8 && !samples_signed) ? 'B' :
            (bits_per_sample == 16 && samples_signed) ? 'h' :
            (bits_per_sample == 16 && !samples_signed) ? 'H' :
            'b';
        size_t nitems = buffer_length / (bits_per_sample / 8);
        result[1] = mp_obj_new_memoryview(typecode, nitems, result_buf);
    }
""",
        """    mp_obj_t result[2] = {mp_obj_new_int_from_uint(gbr), mp_const_none};

    if (gbr != GET_BUFFER_ERROR) {
""" + MARKER + """
        // copies the data because the gc semantics of get_buffer are unclear
        void *result_buf = m_malloc_without_collect(buffer_length);
        memcpy(result_buf, buffer, buffer_length);
        // Byte view: len(buf) is bytes, matching the C protocol's
        // buffer_length and audiodsp's own audiocore.get_buffer.
        result[1] = mp_obj_new_memoryview('B', buffer_length, result_buf);
/* >>> audiodsp-cp end */
    }
""",
    ),
]


def main():
    cp_dir = Path(sys.argv[1])
    mode = sys.argv[2]
    pending = 0
    for relative, before, after in REPLACEMENTS:
        path = cp_dir / relative
        if not path.is_file():
            raise SystemExit("not found: %s" % path)
        text = path.read_text()
        if MARKER in text:
            print("  patched:   %s" % relative)
            continue
        if before not in text:
            raise SystemExit(
                "%s: neither audiodsp's marker nor the upstream text is there "
                "any more - the file changed upstream, and this rewrite needs "
                "a person to re-read it" % relative)
        pending += 1
        if mode == "status":
            print("  pending:   %s" % relative)
        elif mode == "dry-run":
            print("  [dry-run] rewrite %s" % relative)
        else:
            path.write_text(text.replace(before, after, 1))
            print("  rewrote:   %s" % relative)
    if not pending:
        print("  every rewrite is already in place")


main()
