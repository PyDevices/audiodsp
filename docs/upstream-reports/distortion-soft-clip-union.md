# Draft: `soft_clip=False` turns soft clipping on

**Note to the poster — strip everything above the `---`.**

One line, and the symptom is neat enough to lead with: **you cannot turn
`soft_clip` off by passing `False`.** Omitting it works, passing `False`
usually does not. Suggested title:

> `audiofilters.Distortion(soft_clip=False)` enables soft clipping

I scanned every `MP_ARG_BOOL` in `shared-bindings/` for the same mistake; this
is the only real instance. Worth saying so in the thread if anyone asks
whether it is systemic.

The result is undefined rather than merely inverted — it depends on the stack
— so a maintainer may see `False` where the repro below shows `True`. If that
happens, the argument to make is the code, not the output: reading `.u_obj`
from a slot only `.u_bool` was written to is the bug whatever it happens to
return. The neighbouring `samples_signed` on the very next line reads
`.u_bool` correctly, which is the tell.

Verified present on `main` 2026-08-27.

---

## `audiofilters.Distortion(soft_clip=False)` enables soft clipping

`shared-bindings/audiofilters/Distortion.c`. The argument is declared as a
bool:

```c
        { MP_QSTR_soft_clip, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
```

and read back as an object:

```c
    common_hal_audiofilters_distortion_construct(self,
        args[ARG_drive].u_obj,
        ...
        args[ARG_soft_clip].u_obj,        // <-- .u_bool
        args[ARG_mix].u_obj,
        args[ARG_buffer_size].u_int,
        bits_per_sample,
        args[ARG_samples_signed].u_bool,  // this one is right
        ...
```

The parameter is `bool soft_clip`, so an `mp_obj_t` is being converted to
`bool` — i.e. "is this pointer non-null".

`mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)]` is an uninitialised local,
and `mp_arg_parse_all()` treats supplied and defaulted arguments differently
(`py/argcheck.c`):

```c
            if (kw == NULL) {
                ...
                out_vals[i] = allowed[i].defval;   // whole union, zero-filled
                continue;
            }
    ...
        if ((allowed[i].flags & MP_ARG_KIND_MASK) == MP_ARG_BOOL) {
            out_vals[i].u_bool = mp_obj_is_true(given_arg);   // one byte only
        }
```

So:

- **Omitted** — the whole union is copied from the statically zero-filled
  default, `.u_obj` is `NULL`, and `soft_clip` comes out `False`. Right by
  accident.
- **`soft_clip=True`** — one byte set to 1, the remaining
  `sizeof(void *) - 1` bytes are stale stack, `.u_obj` is non-null,
  `soft_clip` is `True`. Right by accident.
- **`soft_clip=False`** — one byte set to 0, the rest still stale. `.u_obj` is
  non-null unless those bytes all happen to be zero, so **`soft_clip` comes
  out `True`**.

Passing `False` is the one case that cannot be right except by luck, and it is
the case a user reaches for when they want the hard curve.

It is undefined behaviour, so the answer varies with what was on the stack:
in one script on one binary I get `True` and in another, on the same binary, I
get `False`. It also varies by target — this is how we originally found it,
building for wasm32 where even the *default* read as `True`.

### Fix

```diff
         mode,
-        args[ARG_soft_clip].u_obj,
+        args[ARG_soft_clip].u_bool,
         args[ARG_mix].u_obj,
```

### Repro

```python
# soft_clip is declared MP_ARG_BOOL and read back through .u_obj, so what you
# get depends on whatever was on the stack under that union slot.
import audiofilters

RATE = 8000


def in_a_function():
    return audiofilters.Distortion(soft_clip=False, sample_rate=RATE).soft_clip


print("asked for soft_clip=False")
print("  at module scope   ->",
      audiofilters.Distortion(soft_clip=False, sample_rate=RATE).soft_clip)
print("  inside a function ->", in_a_function())
print()
print("omitted entirely    ->",
      audiofilters.Distortion(sample_rate=RATE).soft_clip)
print("asked for True      ->",
      audiofilters.Distortion(soft_clip=True, sample_rate=RATE).soft_clip)
```

```
asked for soft_clip=False
  at module scope   -> True
  inside a function -> True

omitted entirely    -> False
asked for True      -> True
```

It is not only the property readback — the flag reaches the audio path, so a
`Distortion` asked for hard clipping renders the soft curve.
