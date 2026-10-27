// The graph's OVERDRIVE node. See audiodsp_graph_overdrive.h.
// SPDX-License-Identifier: MIT

// First, so no multiply-add here is fused on one target and not another: a
// graph renders the same samples on every board (audiodsp_fp_contract.h).
#include "shared/audiodsp_fp_contract.h"

#include "shared/audiodsp_graph_overdrive.h"

#include "shared/audiodsp_biquad.h"
#include "shared/audiodsp_distortion.h"
#include "shared/audiodsp_graph.h"
#include "shared/audiodsp_synth_dsp.h"

#include <math.h>

// OverdriveCore's tone control: synthio.Biquad(HIGH_SHELF, 1500.0, Q=0.7071).
#define OVERDRIVE_SHELF_MODE 6
#define OVERDRIVE_SHELF_HZ 1500.0
#define OVERDRIVE_SHELF_Q 0.7071

typedef struct {
    // audiofilters.Distortion's gains, linear, and the shelf after it: the
    // coefficients synthio.Biquad would hold and its filter state.
    double pre_gain;
    double post_gain;
    audiodsp_biquad_cp_coefficients_t shelf;
    audiodsp_biquad_state_t state;
} graph_overdrive_t;

size_t audiodsp_graph_overdrive_size(void) {
    return sizeof(graph_overdrive_t);
}

void audiodsp_graph_overdrive_reset(void *data) {
    audiodsp_biquad_reset(&((graph_overdrive_t *)data)->state);
}

void audiodsp_graph_overdrive_configure(void *data, const float *param,
    uint32_t sample_rate) {
    graph_overdrive_t *d = data;
    // OverdriveCore.render, term for term, in double as it is: drive sets a
    // pre-gain of up to 20 dB, level a post-gain in dB, and tone the shelf's
    // gain from -12 dB to +12 dB. The two gains are clamped as
    // audiofilters.Distortion clamps them.
    const double drive = (double)param[AUDIODSP_GRAPH_OVERDRIVE_DRIVE];
    const double tone = (double)param[AUDIODSP_GRAPH_OVERDRIVE_TONE];
    double level = (double)param[AUDIODSP_GRAPH_OVERDRIVE_LEVEL];
    if (!(level > 1e-4)) {
        level = 1e-4;
    }
    double pre_db = 20.0 * drive;
    pre_db = pre_db < -60.0 ? -60.0 : (pre_db > 60.0 ? 60.0 : pre_db);
    double post_db = 20.0 * log10(level);
    post_db = post_db < -80.0 ? -80.0 : (post_db > 24.0 ? 24.0 : post_db);
    d->pre_gain = exp(pre_db * 0.11512925464970228420089957273422);
    d->post_gain = exp(post_db * 0.11512925464970228420089957273422);
    const double t = tone < 0.0 ? 0.0 : (tone > 1.0 ? 1.0 : tone);
    const double db = (t - 0.5) * 24.0;
    audiodsp_biquad_cp_configure(&d->shelf, OVERDRIVE_SHELF_MODE,
        audiodsp_biquad_cp_w0(OVERDRIVE_SHELF_HZ, sample_rate),
        OVERDRIVE_SHELF_Q, pow(10.0, db / 40.0));
}

void audiodsp_graph_overdrive_run(void *data, const int16_t *in, int16_t *out,
    uint32_t frames) {
    graph_overdrive_t *d = data;
    // audiofilters.Filter's last step: a sample past +-28000 is squeezed
    // toward the rail (SYNTHIO_MIX_DOWN_SCALE(2)).
    const int32_t knee = 0x0fffffff / (32768 * 2 - 28000);
    for (uint32_t i = 0; i < frames; ++i) {
        const int32_t sample = in != NULL ? in[i] : 0;
        // audiofilters.Distortion in OVERDRIVE mode with soft clipping and
        // the mix at 1; the drive is not read in this mode.
        const int32_t clipped = (int16_t)audiodsp_distortion_sample(sample, 0.0,
            d->pre_gain, d->post_gain, AUDIODSP_DISTORTION_OVERDRIVE, true,
            1.0, 0u);
        const int32_t shelved = audiodsp_biquad_cp_sample(&d->shelf, &d->state,
            clipped);
        out[i] = audiodsp_mix_down_sample(shelved, knee, -28000, 28000);
    }
}
