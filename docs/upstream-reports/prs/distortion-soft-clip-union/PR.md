# PR for adafruit/circuitpython #11267

Branch: `fix-distortion-soft-clip-union` (one commit; the patch file beside
this document). Verified against upstream `main` at `d897c15f` on
2026-08-31.

## Title

```
audiofilters.Distortion(soft_clip=False) enables soft clipping
```

## Body

```markdown
Fixes #11267.

`shared-bindings/audiofilters/Distortion.c` declares `soft_clip` as a bool
argument:

    { MP_QSTR_soft_clip, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },

but reads it back through the wrong union member:

    common_hal_audiofilters_distortion_construct(self,
        ...
        args[ARG_soft_clip].u_obj,        // should be .u_bool
        ...

`common_hal_audiofilters_distortion_construct()` takes `bool soft_clip`, so
this converts an `mp_obj_t` to `bool` -- effectively "is this pointer
non-null" -- instead of reading the bool that `mp_arg_parse_all()` actually
wrote into that argument slot.

`args` is an uninitialised local (`mp_arg_val_t args[MP_ARRAY_SIZE(...)]`),
and `py/argcheck.c` only writes the `.u_bool` byte of an `MP_ARG_BOOL` slot
when the argument is supplied; the rest of the union is whatever was on the
stack. Reading `.u_obj` back is undefined behaviour, and in practice comes
out non-null (true) whenever those leftover bytes are non-zero -- which
`soft_clip=False` does not prevent, since only the low byte gets cleared.
`soft_clip=False` is the one call that cannot be right except by luck.

### Repro

```python
import audiofilters


def make(soft_clip):
    return audiofilters.Distortion(soft_clip=soft_clip, sample_rate=8000).soft_clip


print("default        ", audiofilters.Distortion(sample_rate=8000).soft_clip)
print("soft_clip=False", make(False))
print("soft_clip=True ", make(True))
```

Measured on a unix coverage build of `main`:

|                   | before | after |
|-------------------|--------|-------|
| default           | False  | False |
| `soft_clip=False` | **True**   | False |
| `soft_clip=True`  | True   | True  |

Includes a regression test
(`tests/circuitpython/audiofilter_distortion_soft_clip.py`) with the same
three cases; it fails on current `main` and passes with the fix.
```

## Verification transcript (2026-08-31, upstream main d897c15f)

Before (unmodified `main`, unix coverage build):

```
$ ports/unix/build-coverage/micropython tests/circuitpython/audiofilter_distortion_soft_clip.py
default           False
soft_clip=False   True
soft_clip=True    True
```

After (same build, `.u_obj` -> `.u_bool`):

```
default           False
soft_clip=False   False
soft_clip=True    True
```

Regression test round-trip (run-tests.py, unix coverage build): fails on
the unfixed binary, passes on the fixed one.

Pre-commit: all hooks pass on the changed files (uncrustify 0.78.1, ruff,
codespell, end-of-file, trailing-whitespace, translations -- the
translations hook's rewrite of `locale/circuitpython.pot` was partial-run
churn and is not part of the commit).

Known-failing context: `synthio_biquad.py`, `audiofilter_filter_biquads.py`,
`audiofilter_filter_stereo.py`, `audiofilter_filter_stereo_biquads.py` fail
identically on unmodified `main` in this environment (float precision vs.
the checked-in `.exp` files) -- pre-existing, not caused by this change,
same pattern as the peaking-eq-sign PR.

Note on undefined behaviour: because the "before" reading depends on
whatever bytes happen to sit on the stack under the argument union, the
exact value could in principle read `False` on a different compiler or
optimization level even before the fix. That would not make the code less
buggy -- reading `.u_obj` from a slot only `.u_bool` was written to is the
defect regardless of what it happens to return -- but it means the reporter
should expect the "before" number in this table to be this environment's
result, not a universal constant.

## Publish commands (Brad runs these; nothing has been pushed)

```sh
git clone https://github.com/adafruit/circuitpython.git
cd circuitpython
git checkout -b fix-distortion-soft-clip-union
git am /path/to/audiodsp/docs/upstream-reports/prs/distortion-soft-clip-union/0001-Fix-audiofilters.Distortion-soft_clip-False-enabling.patch

gh repo fork adafruit/circuitpython --remote --remote-name fork
git push fork fix-distortion-soft-clip-union
gh pr create --repo adafruit/circuitpython \
  --title "audiofilters.Distortion(soft_clip=False) enables soft clipping" \
  --body-file <body extracted from the Body section above>
```
