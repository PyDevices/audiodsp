// audiograph.Graph bindings for CircuitPython.
//
// The same calls, arguments and refusals as the MicroPython binding
// (src/audiograph/Graph.c), so a graph written for one runs on the other and
// renders the same samples.
//
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <string.h>

#include "shared-bindings/audiograph/Graph.h"
#include "shared-bindings/audiocore/__init__.h"

#include "py/binary.h"
#include "py/runtime.h"
#include "shared/runtime/context_manager_helpers.h"

//| class Graph:
//|     """A whole patch rendered by one C call per block.
//|
//|     A graph owns ``buffers`` blocks of ``frames`` signed 16-bit samples
//|     and a table of up to ``nodes`` nodes. Every node reads some buffers,
//|     writes one, belongs to a group, and holds one kernel's settings
//|     behind up to sixteen float parameters. Nodes run in the order they
//|     were added. 16384 counts (`UNIT`) is 1.0 on every buffer.
//|
//|     A graph is also an audio sample: hand it to an output's ``play()`` or
//|     to a mixer voice and it renders a block each time one is pulled.
//|     Changes made with `set`, `reset` and `enable` wait in the graph's
//|     queue and land at the top of the next block."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         sample_rate: int = 48000,
//|         frames: int = 128,
//|         channel_count: int = 2,
//|         buffers: int = 64,
//|         nodes: int = 64,
//|         queue: int = 0,
//|     ) -> None:
//|         """Create an empty graph. ``frames`` is the block, 1 to 256.
//|         ``queue`` is how many changes can wait for the next block, 1 to
//|         65536; 0 picks the larger of 256 and four per node."""
//|         ...
//|
//|     def deinit(self) -> None:
//|         """Release the graph. It cannot be used after this."""
//|         ...
//|
//|     def __enter__(self) -> Graph:
//|         """No-op used by Context Managers."""
//|         ...
//|
//|     def __exit__(self) -> None:
//|         """Automatically deinitializes when exiting a context."""
//|         ...
//|
//|     sample_rate: int
//|     """The sample rate the graph renders at. (read-only)"""
//|
//|     channel_count: int
//|     """1 or 2. (read-only)"""
//|
//|     bits_per_sample: int
//|     """Always 16. (read-only)"""
//|
//|     def enable(self, mask: int) -> None:
//|         """Which groups render while the graph is played as a sample, one
//|         bit per group, from the next block. `ALWAYS` nodes always do."""
//|         ...
//|
//|     def hold(self) -> None:
//|         """Keep changes back until `commit`, so a batch of them lands on
//|         one block together."""
//|         ...
//|
//|     def commit(self) -> None:
//|         """Release the changes made since `hold` to the next block."""
//|         ...
//|
//|     def sounding(self) -> Tuple[int, int, int, int]:
//|         """``(sounding, frame, applied, pushed)``: the mask of groups whose
//|         envelopes were still sounding after the last block rendered, the
//|         frames rendered so far, and how many changes have been applied
//|         and queued. A change counted in ``pushed`` but not ``applied`` has
//|         not been heard yet."""
//|         ...
//|
//|     def stats(self) -> Dict[str, int]:
//|         """The queue's counts: ``capacity``, ``pushed``, ``applied``,
//|         ``overflows``, ``splits`` (held batches a full queue broke in two),
//|         ``high_water`` and ``frame``."""
//|         ...
//|
//|     def buffer(self) -> int:
//|         """A fresh block buffer's id."""
//|         ...
//|
//|     def add(self, kind: int, group: int, inputs: Sequence[Optional[int]],
//|             output: Optional[int], arg: float = 0.0) -> int:
//|         """Append a node and return its index. ``group`` is 0 to 31 or
//|         `ALWAYS`; an input left ``None`` is open."""
//|         ...
//|
//|     def set(self, node: int, index: int, value: float) -> None:
//|         """Queue a parameter for the next block; the node derives its
//|         settings before it renders."""
//|         ...
//|
//|     def get(self, node: int, index: int) -> float:
//|         """The value `set` last stored, whether or not a block has heard
//|         it yet."""
//|         ...
//|
//|     def table(self, node: int, table: array.array) -> None:
//|         """A feedback delay's wow shape, an int16 array, borrowed."""
//|         ...
//|
//|     def reset(self, node: int) -> None:
//|         """Clear a node's running state at the next block."""
//|         ...
//|
//|     def run(self, out: Optional[WriteableBuffer] = None, block: int = 0,
//|             enable: int = 0xFFFFFFFF) -> int:
//|         """Render one block into ``out`` at block ``block`` and return the
//|         mask of groups whose envelopes are still sounding."""
//|         ...
//|
//|     def peak(self, buffer: int) -> int:
//|         """A block's peak."""
//|         ...
//|
//|     def read(self, buffer: int, dst: WriteableBuffer) -> None:
//|         """Copy a block out into an int16 buffer of ``frames``."""
//|         ...
//|
//|     def write(self, buffer: int, src: ReadableBuffer) -> None:
//|         """Fill a block from an int16 buffer of ``frames``."""
//|         ...
//|

static audiograph_graph_obj_t *graph_self(mp_obj_t self_in) {
    audiograph_graph_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return self;
}

// No lock: the output pulls on this same thread, between bytecodes, so a
// full queue drains inline.
static void graph_push(audiograph_graph_obj_t *self, uint8_t op,
    uint32_t node, uint32_t index, float value, uint32_t mask) {
    audiodsp_graph_enqueue(&self->graph, op, node, index, value, mask,
        NULL, NULL);
}

static mp_obj_t audiograph_graph_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_sample_rate, ARG_frames, ARG_channel_count, ARG_buffers,
           ARG_nodes, ARG_queue };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sample_rate, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 48000} },
        { MP_QSTR_frames, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 128} },
        { MP_QSTR_channel_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 2} },
        { MP_QSTR_buffers, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 64} },
        { MP_QSTR_nodes, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 64} },
        { MP_QSTR_queue, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t frames = args[ARG_frames].u_int;
    if (frames < 1 || frames > (mp_int_t)AUDIODSP_GRAPH_MAX_FRAMES) {
        mp_raise_ValueError(MP_ERROR_TEXT("frames must be from 1 to 256"));
    }
    mp_int_t channels = args[ARG_channel_count].u_int;
    if (channels < 1 || channels > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel_count must be 1 or 2"));
    }
    mp_int_t buffers = args[ARG_buffers].u_int;
    mp_int_t nodes = args[ARG_nodes].u_int;
    if (buffers < 1 || buffers > 4096 || nodes < 1 || nodes > 4096) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffers and nodes must be from 1 to 4096"));
    }
    if (args[ARG_sample_rate].u_int < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate must be at least 1"));
    }
    mp_int_t queue = args[ARG_queue].u_int;
    if (queue == 0) {
        queue = 4 * nodes > (mp_int_t)AUDIODSP_GRAPH_QUEUE_MIN
            ? 4 * nodes : (mp_int_t)AUDIODSP_GRAPH_QUEUE_MIN;
    }
    if (queue < 1 || queue > 65536) {
        mp_raise_ValueError(MP_ERROR_TEXT("queue must be from 1 to 65536"));
    }
    audiograph_graph_obj_t *self = mp_obj_malloc(audiograph_graph_obj_t, type);
    common_hal_audiograph_graph_construct(self,
        (uint32_t)args[ARG_sample_rate].u_int, (uint32_t)frames,
        (uint32_t)channels, (uint32_t)buffers, (uint32_t)nodes,
        (uint32_t)queue);
    return MP_OBJ_FROM_PTR(self);
}

// buffer(): a fresh block buffer's id.
static mp_obj_t audiograph_graph_buffer(mp_obj_t self_in) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    if (self->next_buffer >= self->graph.nbufs) {
        mp_raise_ValueError(MP_ERROR_TEXT("no buffers left"));
    }
    return MP_OBJ_NEW_SMALL_INT(self->next_buffer++);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiograph_graph_buffer_obj,
    audiograph_graph_buffer);

static int16_t buffer_id(mp_obj_t obj) {
    if (obj == mp_const_none) {
        return AUDIODSP_GRAPH_NONE;
    }
    mp_int_t v = mp_obj_get_int(obj);
    if (v < 0 || v > 32767) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer id"));
    }
    return (int16_t)v;
}

// add(kind, group, inputs, output, arg=0.0): the node's index. `inputs` is
// a sequence of buffer ids (None for an open input), `output` a buffer id
// or None.
static mp_obj_t audiograph_graph_add(size_t n_args, const mp_obj_t *args) {
    audiograph_graph_obj_t *self = graph_self(args[0]);
    mp_int_t kind = mp_obj_get_int(args[1]);
    if (kind < 0 || !audiodsp_graph_kind_built((audiodsp_graph_kind_t)kind)) {
        mp_raise_ValueError(MP_ERROR_TEXT("unknown node kind"));
    }
    mp_int_t group = mp_obj_get_int(args[2]);
    if (group < 0 || (group > 31 && group != AUDIODSP_GRAPH_ALWAYS)) {
        mp_raise_ValueError(MP_ERROR_TEXT("group must be 0..31 or ALWAYS"));
    }
    size_t len = 0;
    mp_obj_t *items = NULL;
    mp_obj_get_array(args[3], &len, &items);
    if (len > AUDIODSP_GRAPH_MAX_INPUTS) {
        mp_raise_ValueError(MP_ERROR_TEXT("too many inputs"));
    }
    int16_t inputs[AUDIODSP_GRAPH_MAX_INPUTS];
    for (size_t i = 0; i < len; ++i) {
        inputs[i] = buffer_id(items[i]);
    }
    int16_t output = buffer_id(args[4]);
    float arg = n_args > 5 ? (float)mp_obj_get_float(args[5]) : 0.0f;
    int index = common_hal_audiograph_graph_add(self,
        (audiodsp_graph_kind_t)kind, (uint32_t)group, inputs, (uint32_t)len,
        output, arg);
    if (index < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("graph is full, a buffer id is out of range, or the node has no output"));
    }
    return MP_OBJ_NEW_SMALL_INT(index);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiograph_graph_add_obj, 5, 6,
    audiograph_graph_add);

static mp_obj_t audiograph_graph_set(size_t n_args, const mp_obj_t *args) {
    audiograph_graph_obj_t *self = graph_self(args[0]);
    (void)n_args;
    mp_int_t node = mp_obj_get_int(args[1]);
    mp_int_t index = mp_obj_get_int(args[2]);
    float value = (float)mp_obj_get_float(args[3]);
    if (node < 0 || index < 0 ||
        !audiodsp_graph_valid(&self->graph, (uint32_t)node, (uint32_t)index)) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad node or parameter index"));
    }
    graph_push(self, AUDIODSP_GRAPH_OP_SET, (uint32_t)node, (uint32_t)index,
        value, 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiograph_graph_set_obj, 4, 4,
    audiograph_graph_set);

// _set_now(node, index, value): writes the parameter straight into the node,
// past the queue. Not for use: it exists so the pump probe can show its check
// catches a write that skips the queue.
static mp_obj_t audiograph_graph_set_now(size_t n_args, const mp_obj_t *args) {
    audiograph_graph_obj_t *self = graph_self(args[0]);
    (void)n_args;
    mp_int_t node = mp_obj_get_int(args[1]);
    mp_int_t index = mp_obj_get_int(args[2]);
    if (node < 0 || index < 0 || !audiodsp_graph_set(&self->graph,
            (uint32_t)node, (uint32_t)index, (float)mp_obj_get_float(args[3]))) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad node or parameter index"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiograph_graph_set_now_obj, 4, 4,
    audiograph_graph_set_now);

static mp_obj_t audiograph_graph_get(mp_obj_t self_in, mp_obj_t node,
    mp_obj_t index) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    float value = 0.0f;
    mp_int_t n = mp_obj_get_int(node);
    mp_int_t i = mp_obj_get_int(index);
    if (n < 0 || i < 0 ||
        !audiodsp_graph_get_shadow(&self->graph, (uint32_t)n, (uint32_t)i, &value)) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad node or parameter index"));
    }
    return mp_obj_new_float(value);
}
static MP_DEFINE_CONST_FUN_OBJ_3(audiograph_graph_get_obj,
    audiograph_graph_get);

// table(node, int16_array): a feedback delay's wow shape, borrowed.
static mp_obj_t audiograph_graph_table(mp_obj_t self_in, mp_obj_t node,
    mp_obj_t table) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    mp_buffer_info_t info;
    mp_get_buffer_raise(table, &info, MP_BUFFER_READ);
    if (info.typecode != 'h' || info.len % 2) {
        mp_raise_TypeError(MP_ERROR_TEXT("table must be an int16 array"));
    }
    if (!audiodsp_graph_set_table(&self->graph, (uint32_t)mp_obj_get_int(node),
            (const int16_t *)info.buf, (uint32_t)(info.len / 2))) {
        mp_raise_ValueError(MP_ERROR_TEXT("not a delay node, or a bad table length"));
    }
    mp_obj_list_append(self->keep, table);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(audiograph_graph_table_obj,
    audiograph_graph_table);

static mp_obj_t audiograph_graph_reset(mp_obj_t self_in, mp_obj_t node) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    mp_int_t n = mp_obj_get_int(node);
    if (n < 0 || !audiodsp_graph_valid(&self->graph, (uint32_t)n, 0)) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad node"));
    }
    graph_push(self, AUDIODSP_GRAPH_OP_RESET, (uint32_t)n, 0, 0.0f, 0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiograph_graph_reset_obj,
    audiograph_graph_reset);

static mp_obj_t audiograph_graph_enable(mp_obj_t self_in, mp_obj_t mask) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    graph_push(self, AUDIODSP_GRAPH_OP_ENABLE, 0, 0, 0.0f,
        (uint32_t)mp_obj_get_int_truncated(mask));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiograph_graph_enable_obj,
    audiograph_graph_enable);

static mp_obj_t audiograph_graph_hold(mp_obj_t self_in) {
    audiodsp_graph_hold(&graph_self(self_in)->graph);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiograph_graph_hold_obj,
    audiograph_graph_hold);

static mp_obj_t audiograph_graph_commit(mp_obj_t self_in) {
    audiodsp_graph_commit(&graph_self(self_in)->graph);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiograph_graph_commit_obj,
    audiograph_graph_commit);

static mp_obj_t audiograph_graph_sounding(mp_obj_t self_in) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    uint32_t sounding, frame, applied;
    audiodsp_graph_published(&self->graph, &sounding, &frame, &applied);
    mp_obj_t items[4] = {
        mp_obj_new_int_from_uint(sounding),
        mp_obj_new_int_from_uint(frame),
        mp_obj_new_int_from_uint(applied),
        mp_obj_new_int_from_uint(self->graph.pushed),
    };
    return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiograph_graph_sounding_obj,
    audiograph_graph_sounding);

static mp_obj_t audiograph_graph_stats(mp_obj_t self_in) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    const audiodsp_graph_t *g = &self->graph;
    uint32_t sounding, frame, applied;
    audiodsp_graph_published(g, &sounding, &frame, &applied);
    mp_obj_t d = mp_obj_new_dict(7);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_capacity), mp_obj_new_int_from_uint(g->capacity));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_pushed), mp_obj_new_int_from_uint(g->pushed));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_applied), mp_obj_new_int_from_uint(applied));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_overflows), mp_obj_new_int_from_uint(g->overflows));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_splits), mp_obj_new_int_from_uint(g->splits));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_high_water), mp_obj_new_int_from_uint(g->high_water));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_frame), mp_obj_new_int_from_uint(frame));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiograph_graph_stats_obj,
    audiograph_graph_stats);

static mp_obj_t audiograph_graph_deinit(mp_obj_t self_in) {
    audiograph_graph_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_mark_deinit(&self->base);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiograph_graph_deinit_obj,
    audiograph_graph_deinit);

// run(out=None, block=0, enable=0xFFFFFFFF): renders one block into `out`
// at block `block` (frames * channel_count samples each), and returns the
// mask of groups whose envelopes are still sounding.
static mp_obj_t audiograph_graph_run(size_t n_args, const mp_obj_t *args) {
    audiograph_graph_obj_t *self = graph_self(args[0]);
    int16_t *out = NULL;
    if (n_args > 1 && args[1] != mp_const_none) {
        mp_buffer_info_t info;
        mp_get_buffer_raise(args[1], &info, MP_BUFFER_WRITE);
        mp_int_t block = n_args > 2 ? mp_obj_get_int(args[2]) : 0;
        size_t block_bytes = (size_t)self->graph.frames * self->graph.channel_count * 2u;
        if (block < 0 || (size_t)(block + 1) * block_bytes > info.len) {
            mp_raise_ValueError(MP_ERROR_TEXT("out is too small for that block"));
        }
        out = (int16_t *)(void *)((uint8_t *)info.buf + (size_t)block * block_bytes);
    }
    uint32_t sounding;
    if (n_args > 3) {
        sounding = audiodsp_graph_render_mask(&self->graph, out,
            (uint32_t)mp_obj_get_int_truncated(args[3]));
    } else {
        sounding = audiodsp_graph_render(&self->graph, out);
    }
    return mp_obj_new_int_from_uint(sounding);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiograph_graph_run_obj, 1, 4,
    audiograph_graph_run);

static mp_obj_t audiograph_graph_peak(mp_obj_t self_in, mp_obj_t buffer) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    int32_t peak = audiodsp_graph_peak(&self->graph, (uint32_t)mp_obj_get_int(buffer));
    if (peak < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer id"));
    }
    return MP_OBJ_NEW_SMALL_INT(peak);
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiograph_graph_peak_obj,
    audiograph_graph_peak);

static int16_t *block_of(audiograph_graph_obj_t *self, mp_obj_t buffer) {
    mp_int_t id = mp_obj_get_int(buffer);
    if (id < 0 || (uint32_t)id >= self->graph.nbufs) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad buffer id"));
    }
    return self->graph.bufs[id];
}

// read(buffer, dst): copy a block out into an int16 buffer of `frames`.
static mp_obj_t audiograph_graph_read(mp_obj_t self_in, mp_obj_t buffer,
    mp_obj_t dst) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    const int16_t *src = block_of(self, buffer);
    mp_buffer_info_t info;
    mp_get_buffer_raise(dst, &info, MP_BUFFER_WRITE);
    if (info.len != (size_t)self->graph.frames * 2u) {
        mp_raise_ValueError(MP_ERROR_TEXT("dst must hold one block of int16"));
    }
    memcpy(info.buf, src, info.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(audiograph_graph_read_obj,
    audiograph_graph_read);

// write(buffer, src): fill a block from an int16 buffer of `frames`.
static mp_obj_t audiograph_graph_write(mp_obj_t self_in, mp_obj_t buffer,
    mp_obj_t src) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    int16_t *dst = block_of(self, buffer);
    mp_buffer_info_t info;
    mp_get_buffer_raise(src, &info, MP_BUFFER_READ);
    if (info.len != (size_t)self->graph.frames * 2u) {
        mp_raise_ValueError(MP_ERROR_TEXT("src must hold one block of int16"));
    }
    memcpy(dst, info.buf, info.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(audiograph_graph_write_obj,
    audiograph_graph_write);

static const mp_rom_map_elem_t audiograph_graph_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiograph_graph_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_enable), MP_ROM_PTR(&audiograph_graph_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_hold), MP_ROM_PTR(&audiograph_graph_hold_obj) },
    { MP_ROM_QSTR(MP_QSTR_commit), MP_ROM_PTR(&audiograph_graph_commit_obj) },
    { MP_ROM_QSTR(MP_QSTR_sounding), MP_ROM_PTR(&audiograph_graph_sounding_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&audiograph_graph_stats_obj) },
    AUDIOSAMPLE_FIELDS,
    { MP_ROM_QSTR(MP_QSTR_buffer), MP_ROM_PTR(&audiograph_graph_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_add), MP_ROM_PTR(&audiograph_graph_add_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&audiograph_graph_set_obj) },
    { MP_ROM_QSTR(MP_QSTR__set_now), MP_ROM_PTR(&audiograph_graph_set_now_obj) },
    { MP_ROM_QSTR(MP_QSTR_get), MP_ROM_PTR(&audiograph_graph_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_table), MP_ROM_PTR(&audiograph_graph_table_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&audiograph_graph_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_run), MP_ROM_PTR(&audiograph_graph_run_obj) },
    { MP_ROM_QSTR(MP_QSTR_peak), MP_ROM_PTR(&audiograph_graph_peak_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&audiograph_graph_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&audiograph_graph_write_obj) },
};
static MP_DEFINE_CONST_DICT(audiograph_graph_locals,
    audiograph_graph_locals_table);

static const audiosample_p_t audiograph_graph_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = (audiosample_reset_buffer_fun)
        audiograph_graph_reset_buffer,
    .get_buffer = (audiosample_get_buffer_fun)
        audiograph_graph_get_buffer,
};

MP_DEFINE_CONST_OBJ_TYPE(
    audiograph_graph_type,
    MP_QSTR_Graph,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiograph_graph_make_new,
    locals_dict, &audiograph_graph_locals,
    protocol, &audiograph_graph_proto
    );
