// audiograph.Graph. See Graph.h.
// SPDX-License-Identifier: MIT

#include "audiograph/Graph.h"

#include <string.h>

#include "audiopump/audiopump.h"
#include "cp_compat/context_manager_helpers.h"
#include "py/binary.h"
#include "py/runtime.h"
#include "shared/audiodsp_pump_lock.h"

// A table page's size: small enough to fit a fragmented heap.
#define AUDIOGRAPH_PAGE_BYTES 4096u

// A live graph, or a raise.
static audiograph_graph_obj_t *graph_self(mp_obj_t self_in) {
    audiograph_graph_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiosample_check_for_deinit(&self->base);
    return self;
}

// The same, for a call that renders or rewires the graph: two renderers, or
// a node added while the pump walks the table, would race.
static audiograph_graph_obj_t *graph_idle(mp_obj_t self_in) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    if (self->pumped && audiopump_is_running()) {
        mp_raise_msg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("the pump is rendering this graph"));
    }
    return self;
}

static void graph_push(audiograph_graph_obj_t *self, uint8_t op,
    uint32_t node, uint32_t index, float value, uint32_t mask) {
    audiodsp_graph_enqueue(&self->graph, op, node, index, value, mask,
        audiodsp_pump_lock_acquire, audiodsp_pump_lock_release);
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
    // Every table goes in pages of a few KB, so a large patch still fits a
    // fragmented heap. The GC keeps a block alive only through a pointer to
    // its start, so the first pointer into each page is that start.
    size_t per = AUDIOGRAPH_PAGE_BYTES / ((size_t)frames * sizeof(int16_t));
    per = per ? per : 1;
    int16_t **bufs = m_new(int16_t *, (size_t)buffers);
    for (mp_int_t i = 0; i < buffers; i += (mp_int_t)per) {
        size_t n = (size_t)(buffers - i) < per ? (size_t)(buffers - i) : per;
        int16_t *page = m_new0(int16_t, n * (size_t)frames);
        for (size_t k = 0; k < n; ++k) {
            bufs[i + (mp_int_t)k] = page + k * (size_t)frames;
        }
    }
    uint32_t node_pages = audiodsp_graph_pages((uint32_t)nodes, AUDIODSP_GRAPH_NODE_PAGE);
    audiodsp_graph_node_t **table = m_new(audiodsp_graph_node_t *, node_pages);
    float **shadow = m_new(float *, node_pages);
    for (uint32_t i = 0; i < node_pages; ++i) {
        table[i] = m_new0(audiodsp_graph_node_t, AUDIODSP_GRAPH_NODE_PAGE);
        shadow[i] = m_new0(float, AUDIODSP_GRAPH_SHADOW_PAGE);
    }
    uint32_t record_pages = audiodsp_graph_pages((uint32_t)queue, AUDIODSP_GRAPH_RECORD_PAGE);
    audiodsp_graph_record_t **records = m_new(audiodsp_graph_record_t *, record_pages);
    for (uint32_t i = 0; i < record_pages; ++i) {
        records[i] = m_new0(audiodsp_graph_record_t, AUDIODSP_GRAPH_RECORD_PAGE);
    }
    audiodsp_graph_init(&self->graph, (uint32_t)args[ARG_sample_rate].u_int,
        (uint32_t)frames, (uint32_t)channels, bufs, (uint32_t)buffers, table,
        (uint32_t)nodes);
    audiodsp_graph_queue_init(&self->graph, records, (uint32_t)queue, shadow);
    self->out = m_new0(int16_t, 2u * (size_t)frames * (size_t)channels);
    self->which = 0;
    self->pumped = false;
    self->base.sample_rate = (uint32_t)args[ARG_sample_rate].u_int;
    self->base.max_buffer_length = (uint32_t)frames * (uint32_t)channels * 2u;
    self->base.bits_per_sample = 16;
    self->base.channel_count = (uint8_t)channels;
    self->base.samples_signed = 1;
    self->base.single_buffer = false;
    self->next_buffer = 0;
    self->keep = mp_obj_new_list(0, NULL);
    return MP_OBJ_FROM_PTR(self);
}

// buffer(): a fresh block buffer's id.
static mp_obj_t audiograph_graph_buffer(mp_obj_t self_in) {
    audiograph_graph_obj_t *self = graph_idle(self_in);
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
    audiograph_graph_obj_t *self = graph_idle(args[0]);
    mp_int_t kind = mp_obj_get_int(args[1]);
    if (kind < 0 || kind >= (mp_int_t)AUDIODSP_GRAPH_KINDS) {
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
    size_t size = audiodsp_graph_node_data_size((audiodsp_graph_kind_t)kind,
        self->graph.sample_rate, arg);
    void *data = size ? m_new0(uint8_t, size) : NULL;
    int index = audiodsp_graph_add(&self->graph, (audiodsp_graph_kind_t)kind,
        (uint32_t)group, inputs, (uint32_t)len, output, data, arg);
    if (index < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("graph is full, a buffer id is out of range, or the node has no output"));
    }
    return MP_OBJ_NEW_SMALL_INT(index);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiograph_graph_add_obj, 5, 6,
    audiograph_graph_add);

// set(node, index, value): queued; the renderer applies it at the top of its
// next block, or with the rest of a held batch.
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
// past the queue, racing whatever renders. Not for use: it exists so the
// pump probe can show its check catches a write that skips the queue.
static mp_obj_t audiograph_graph_set_now(size_t n_args, const mp_obj_t *args) {
    audiograph_graph_obj_t *self = graph_self(args[0]);
    (void)n_args;
    if (!audiodsp_graph_set(&self->graph, (uint32_t)mp_obj_get_int(args[1]),
            (uint32_t)mp_obj_get_int(args[2]), (float)mp_obj_get_float(args[3]))) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad node or parameter index"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiograph_graph_set_now_obj, 4, 4,
    audiograph_graph_set_now);

// get(node, index): the value set() last stored, whoever renders.
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
    return mp_obj_new_float((mp_float_t)value);
}
static MP_DEFINE_CONST_FUN_OBJ_3(audiograph_graph_get_obj,
    audiograph_graph_get);

// table(node, int16_array): a feedback delay's wow shape, borrowed.
static mp_obj_t audiograph_graph_table(mp_obj_t self_in, mp_obj_t node,
    mp_obj_t table) {
    audiograph_graph_obj_t *self = graph_idle(self_in);
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

// reset(node): queued, like set().
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

// carry(node, src_graph, src_node): hands a node of another graph (or of
// this one) over to `node`, its running state and all, so a graph compiled
// beside a running one can take over at a block boundary. Both queues are
// drained first. Refuses a node of another kind, built with another arg, of
// another size, or from a graph at another sample rate.
static mp_obj_t audiograph_graph_carry(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    audiograph_graph_obj_t *self = graph_idle(args[0]);
    if (!mp_obj_is_type(args[2], &audiograph_graph_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("src_graph must be a Graph"));
    }
    audiograph_graph_obj_t *src = graph_idle(args[2]);
    mp_int_t node = mp_obj_get_int(args[1]);
    mp_int_t src_node = mp_obj_get_int(args[3]);
    if (node < 0 || src_node < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad node"));
    }
    audiodsp_graph_drain(&src->graph);
    audiodsp_graph_drain(&self->graph);
    switch (audiodsp_graph_carry(&self->graph, (uint32_t)node, &src->graph,
        (uint32_t)src_node)) {
        case AUDIODSP_GRAPH_CARRY_OK:
            break;
        case AUDIODSP_GRAPH_CARRY_KIND:
            mp_raise_ValueError(MP_ERROR_TEXT("carry: the nodes are of different kinds"));
        case AUDIODSP_GRAPH_CARRY_ARG:
            mp_raise_ValueError(MP_ERROR_TEXT("carry: the nodes were built with different args"));
        case AUDIODSP_GRAPH_CARRY_SIZE:
            mp_raise_ValueError(MP_ERROR_TEXT("carry: the nodes' state differs in size"));
        case AUDIODSP_GRAPH_CARRY_RATE:
            mp_raise_ValueError(MP_ERROR_TEXT("carry: the graphs run at different sample rates"));
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("bad node"));
    }
    // a delay may still be playing the source's wow table: keep it alive here
    if (src != self) {
        size_t len = 0;
        mp_obj_t *items = NULL;
        mp_obj_list_get(src->keep, &len, &items);
        for (size_t i = 0; i < len; ++i) {
            mp_obj_list_append(self->keep, items[i]);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiograph_graph_carry_obj, 4, 4,
    audiograph_graph_carry);

// enable(mask): the groups a pull renders, queued like set(), so a note's
// gate and its group's enable land in the same block.
static mp_obj_t audiograph_graph_enable(mp_obj_t self_in, mp_obj_t mask) {
    audiograph_graph_obj_t *self = graph_self(self_in);
    graph_push(self, AUDIODSP_GRAPH_OP_ENABLE, 0, 0, 0.0f,
        (uint32_t)mp_obj_get_int_truncated(mask));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(audiograph_graph_enable_obj,
    audiograph_graph_enable);

// hold() and commit(): what is queued between them lands in one block.
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

// sounding(): (groups sounding, frames rendered, records applied, records
// queued), as the renderer last published them; the last is this side's.
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

// stats(): the queue's counts, as a dict.
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

// run(out=None, block=0, enable=0xFFFFFFFF): renders one block into `out`
// at block `block` (frames * channel_count samples each), and returns the
// mask of groups whose envelopes are still sounding.
static mp_obj_t audiograph_graph_run(size_t n_args, const mp_obj_t *args) {
    audiograph_graph_obj_t *self = graph_idle(args[0]);
    int16_t *out = NULL;
    if (n_args > 1 && args[1] != mp_const_none) {
        mp_buffer_info_t info;
        mp_get_buffer_raise(args[1], &info, MP_BUFFER_WRITE);
        mp_int_t block = n_args > 2 ? mp_obj_get_int(args[2]) : 0;
        size_t block_bytes = (size_t)self->graph.frames * self->graph.channel_count * 2u;
        if (block < 0 || (size_t)(block + 1) * block_bytes > info.len) {
            mp_raise_ValueError(MP_ERROR_TEXT("out is too small for that block"));
        }
        out = (int16_t *)((uint8_t *)info.buf + (size_t)block * block_bytes);
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
    audiograph_graph_obj_t *self = graph_idle(self_in);
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

// deinit(): marks the graph released, under the pump lock, so a pump pulling
// it stops on it with fault=deinited rather than rendering a freed graph.
static mp_obj_t audiograph_graph_deinit(mp_obj_t self_in) {
    audiograph_graph_obj_t *self = MP_OBJ_TO_PTR(self_in);
    AUDIODSP_PUMP_SWAP(audiosample_mark_deinit(&self->base));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiograph_graph_deinit_obj,
    audiograph_graph_deinit);

// --- the audiosample protocol -------------------------------------------

static void audiograph_graph_reset_buffer(mp_obj_t self_in,
    bool single_channel_output, uint8_t channel) {
    (void)self_in;
    (void)single_channel_output;
    (void)channel;
}

// One block per pull, never DONE: a graph does not end. Called inside the
// pull funnel, so under the pump lock, and never raises.
static audioio_get_buffer_result_t audiograph_graph_get_buffer(
    mp_obj_t self_in, bool single_channel_output, uint8_t channel,
    uint8_t **buffer, uint32_t *buffer_length) {
    audiograph_graph_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const uint32_t samples = self->graph.frames * self->graph.channel_count;
    if (audiodsp_pump_on_pump_thread()) {
        self->pumped = true;
    }
    if (!single_channel_output || channel == 0) {
        self->which ^= 1u;
        audiodsp_graph_render(&self->graph, self->out + self->which * samples);
    }
    int16_t *block = self->out + self->which * samples;
    if (single_channel_output && channel < self->graph.channel_count) {
        block += channel;
    }
    *buffer = (uint8_t *)block;
    *buffer_length = samples * 2u;
    return GET_BUFFER_MORE_DATA;
}

// Nothing behind a graph: the walk ends here.
static mp_obj_t audiograph_graph_sources(mp_obj_t self_in, uint8_t index) {
    (void)self_in;
    (void)index;
    return MP_OBJ_NULL;
}

static const audiosample_p_t audiograph_graph_proto = {
    MP_PROTO_IMPLEMENT(MP_QSTR_protocol_audiosample)
    .reset_buffer = audiograph_graph_reset_buffer,
    .get_buffer = audiograph_graph_get_buffer,
    .sources = audiograph_graph_sources,
};

static const mp_rom_map_elem_t audiograph_graph_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiograph_graph_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&default___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_enable), MP_ROM_PTR(&audiograph_graph_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_hold), MP_ROM_PTR(&audiograph_graph_hold_obj) },
    { MP_ROM_QSTR(MP_QSTR_commit), MP_ROM_PTR(&audiograph_graph_commit_obj) },
    { MP_ROM_QSTR(MP_QSTR_sounding), MP_ROM_PTR(&audiograph_graph_sounding_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&audiograph_graph_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR__set_now), MP_ROM_PTR(&audiograph_graph_set_now_obj) },
    AUDIOSAMPLE_FIELDS,
    { MP_ROM_QSTR(MP_QSTR_buffer), MP_ROM_PTR(&audiograph_graph_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_add), MP_ROM_PTR(&audiograph_graph_add_obj) },
    { MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&audiograph_graph_set_obj) },
    { MP_ROM_QSTR(MP_QSTR_get), MP_ROM_PTR(&audiograph_graph_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_table), MP_ROM_PTR(&audiograph_graph_table_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&audiograph_graph_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_carry), MP_ROM_PTR(&audiograph_graph_carry_obj) },
    { MP_ROM_QSTR(MP_QSTR_run), MP_ROM_PTR(&audiograph_graph_run_obj) },
    { MP_ROM_QSTR(MP_QSTR_peak), MP_ROM_PTR(&audiograph_graph_peak_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&audiograph_graph_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&audiograph_graph_write_obj) },
};
static MP_DEFINE_CONST_DICT(audiograph_graph_locals,
    audiograph_graph_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audiograph_graph_type,
    MP_QSTR_Graph,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, audiograph_graph_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiograph_graph_locals,
    protocol, &audiograph_graph_proto
    );
