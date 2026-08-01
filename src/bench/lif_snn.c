/* lif_snn.c -- event-driven LIF inference kernel (float32)
 *
 * Matches snnTorch Leaky with the DEFAULTS used in training:
 *   threshold = 1.0, reset_mechanism = "subtract", reset_delay = True
 *
 * The reset_delay=True recurrence is easy to get wrong. snnTorch computes the
 * reset flag from the PREVIOUS membrane value, so the subtraction lands one
 * step after the threshold crossing:
 *
 *     reset = (mem_prev > 1.0)
 *     mem   = beta * mem_prev + input - reset * 1.0
 *     spike = (mem > 1.0)
 *
 * Applying the reset immediately (the "obvious" port) diverges after the first
 * spike. Verified against PyTorch golden vectors in test_host.c.
 */

#include "lif_snn.h"
#include SNN_MODEL_HEADER

/* Layers 2 and 3 take BINARY input, so their MACs are skipped for silent
 * neurons. That skipping is the entire energy argument -- a dense port would
 * measure the same joules as an ANN and the paper would have nothing to show.
 * Layer 1 sees real-valued sensor data and is unavoidably dense. */

void snn_infer(const float *x_raw, float *out, snn_stats_t *st)
{
    float mem1[SNN_H], mem2[SNN_H];
    float s1[SNN_H], s2[SNN_H];
    float acc[SNN_NOUT];
    unsigned long spikes = 0, macs = 0;

    for (int i = 0; i < SNN_H; ++i) { mem1[i] = 0.0f; mem2[i] = 0.0f; }
    for (int k = 0; k < SNN_NOUT; ++k) acc[k] = 0.0f;

    for (int t = 0; t < SNN_T; ++t) {
        const float *xt = x_raw + t * SNN_NF;
        float xn[SNN_NF];
        for (int k = 0; k < SNN_NF; ++k)
            xn[k] = (xt[k] - snn_mu[k]) / snn_sd[k];

        /* ---- layer 1: dense (real-valued input) ---- */
        for (int i = 0; i < SNN_H; ++i) {
            float in = snn_f1_b[i];
            const float *w = &snn_f1_w[i * SNN_NF];
            for (int k = 0; k < SNN_NF; ++k) in += w[k] * xn[k];
            macs += SNN_NF;

            float reset = (mem1[i] > SNN_THRESH) ? SNN_THRESH : 0.0f;
            mem1[i] = SNN_BETA * mem1[i] + in - reset;
            s1[i]   = (mem1[i] > SNN_THRESH) ? 1.0f : 0.0f;
        }

        /* ---- layer 2: event-driven (binary input) ---- */
        float in2[SNN_H];
        for (int i = 0; i < SNN_H; ++i) in2[i] = snn_f2_b[i];
        for (int j = 0; j < SNN_H; ++j) {
            if (s1[j] == 0.0f) continue;            /* skipped entirely */
            const float *col = &snn_f2_wT[j * SNN_H];
            for (int i = 0; i < SNN_H; ++i) in2[i] += col[i];
            macs += SNN_H;
            spikes++;
        }
        for (int i = 0; i < SNN_H; ++i) {
            float reset = (mem2[i] > SNN_THRESH) ? SNN_THRESH : 0.0f;
            mem2[i] = SNN_BETA * mem2[i] + in2[i] - reset;
            s2[i]   = (mem2[i] > SNN_THRESH) ? 1.0f : 0.0f;
        }

        /* ---- readout: non-spiking leaky integrator ---- */
        for (int k = 0; k < SNN_NOUT; ++k) acc[k] = SNN_BETA_OUT * acc[k] + snn_out_b[k];
        for (int j = 0; j < SNN_H; ++j) {
            if (s2[j] == 0.0f) continue;
            const float *w = &snn_out_wT[j * SNN_NOUT];
            for (int k = 0; k < SNN_NOUT; ++k) acc[k] += w[k];
            macs += SNN_NOUT;
            spikes++;
        }
    }

    if (st) {
        st->spikes = spikes;
        st->macs   = macs;
        st->rate   = (float)spikes / (float)(2 * SNN_T * SNN_H);
    }
    /* de-normalise: training used standardised targets */
    for (int k = 0; k < SNN_NOUT; ++k) out[k] = acc[k] * SNN_Y_SIG + SNN_Y_MU;
}

/* Dense reference: identical maths, no spike skipping. Used to measure what
 * the sparsity is actually worth on real silicon rather than assuming it. */
void snn_infer_dense(const float *x_raw, float *out, snn_stats_t *st)
{
    float mem1[SNN_H], mem2[SNN_H], s1[SNN_H], s2[SNN_H];
    float acc[SNN_NOUT];
    unsigned long macs = 0, spikes = 0;

    for (int i = 0; i < SNN_H; ++i) { mem1[i] = 0.0f; mem2[i] = 0.0f; }
    for (int k = 0; k < SNN_NOUT; ++k) acc[k] = 0.0f;

    for (int t = 0; t < SNN_T; ++t) {
        const float *xt = x_raw + t * SNN_NF;
        float xn[SNN_NF];
        for (int k = 0; k < SNN_NF; ++k)
            xn[k] = (xt[k] - snn_mu[k]) / snn_sd[k];

        for (int i = 0; i < SNN_H; ++i) {
            float in = snn_f1_b[i];
            const float *w = &snn_f1_w[i * SNN_NF];
            for (int k = 0; k < SNN_NF; ++k) in += w[k] * xn[k];
            macs += SNN_NF;
            float reset = (mem1[i] > SNN_THRESH) ? SNN_THRESH : 0.0f;
            mem1[i] = SNN_BETA * mem1[i] + in - reset;
            s1[i]   = (mem1[i] > SNN_THRESH) ? 1.0f : 0.0f;
        }
        for (int i = 0; i < SNN_H; ++i) {
            float in = snn_f2_b[i];
            for (int j = 0; j < SNN_H; ++j) in += snn_f2_wT[j * SNN_H + i] * s1[j];
            macs += SNN_H;
            float reset = (mem2[i] > SNN_THRESH) ? SNN_THRESH : 0.0f;
            mem2[i] = SNN_BETA * mem2[i] + in - reset;
            s2[i]   = (mem2[i] > SNN_THRESH) ? 1.0f : 0.0f;
            spikes += (s1[i] != 0.0f) + (s2[i] != 0.0f);
        }
        for (int k = 0; k < SNN_NOUT; ++k) {
            float o = SNN_BETA_OUT * acc[k] + snn_out_b[k];
            for (int j = 0; j < SNN_H; ++j) { o += snn_out_wT[j * SNN_NOUT + k] * s2[j]; macs++; }
            acc[k] = o;
        }
    }
    if (st) { st->spikes = spikes; st->macs = macs;
              st->rate = (float)spikes / (float)(2 * SNN_T * SNN_H); }
    for (int k = 0; k < SNN_NOUT; ++k) out[k] = acc[k] * SNN_Y_SIG + SNN_Y_MU;
}

/* Neuron-dynamics floor. Same LIF state updates and same timestep loop, but
 * every synaptic accumulation is removed (each neuron gets its bias only).
 * The difference between this and snn_infer is the true synaptic cost; this
 * number is the part of the latency that sparsity can never reduce.
 *
 * Not a valid predictor -- it exists purely to decompose the timing. */
float snn_infer_lif_only(const float *x_raw, snn_stats_t *st)
{
    float mem1[SNN_H], mem2[SNN_H];
    float acc = 0.0f;

    for (int i = 0; i < SNN_H; ++i) { mem1[i] = 0.0f; mem2[i] = 0.0f; }

    for (int t = 0; t < SNN_T; ++t) {
        const float *xt = x_raw + t * SNN_NF;
        float xn[SNN_NF];
        for (int k = 0; k < SNN_NF; ++k)
            xn[k] = (xt[k] - snn_mu[k]) / snn_sd[k];
        acc += xn[0] * 1e-30f;                   /* keep the loop alive */

        for (int i = 0; i < SNN_H; ++i) {
            float reset = (mem1[i] > SNN_THRESH) ? SNN_THRESH : 0.0f;
            mem1[i] = SNN_BETA * mem1[i] + snn_f1_b[i] - reset;
        }
        for (int i = 0; i < SNN_H; ++i) {
            float reset = (mem2[i] > SNN_THRESH) ? SNN_THRESH : 0.0f;
            mem2[i] = SNN_BETA * mem2[i] + snn_f2_b[i] - reset;
        }
        for (int k = 0; k < SNN_NOUT; ++k) acc += snn_out_b[k];
    }
    if (st) { st->spikes = 0; st->macs = 0; st->rate = 0.0f; }
    return acc;
}
