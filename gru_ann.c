/* gru_ann.c -- dense GRU baseline for the forecasting task (Phase D)
 *
 * Fair-comparison rules, same as the soiling MLP:
 *   - same raw 12 x 7 window as the SNN kernel, normalisation done here
 *   - hand-written float32 C, no interpreter, same style as lif_snn.c
 *
 * PyTorch nn.GRU gate order is (r, z, n) and the reset gate is applied to the
 * hidden contribution AFTER its bias is added:
 *
 *     r = sigma(W_ir x + b_ir + W_hr h + b_hr)
 *     z = sigma(W_iz x + b_iz + W_hz h + b_hz)
 *     n = tanh (W_in x + b_in + r * (W_hn h + b_hn))
 *     h = (1 - z) * n + z * h
 *
 * Writing `r * W_hn h + b_hn` instead compiles fine and drifts slowly from the
 * reference -- the same class of silent bug as the SNN's reset_delay. Verified
 * against PyTorch golden vectors.
 */

#include <math.h>
#include "gru_ann.h"
#include "forecast_gru.h"

/* Rows of weight_ih_l0 / weight_hh_l0 are stacked [r; z; n], H rows each. */
#define R_OFF (0 * GRU_H)
#define Z_OFF (1 * GRU_H)
#define N_OFF (2 * GRU_H)

static inline float sigmoidf(float x) { return 1.0f / (1.0f + expf(-x)); }

void gru_infer(const float *x_raw, float *out, gru_stats_t *st)
{
    float h[GRU_H];
    float gi[3 * GRU_H], gh[3 * GRU_H];
    unsigned long macs = 0, nonlin = 0;

    for (int i = 0; i < GRU_H; ++i) h[i] = 0.0f;

    for (int t = 0; t < GRU_T; ++t) {
        const float *xt = x_raw + t * GRU_NF;
        float xn[GRU_NF];
        for (int k = 0; k < GRU_NF; ++k)
            xn[k] = (xt[k] - gru_mu[k]) / gru_sd[k];

        /* input and hidden projections: both are dense every timestep --
         * a GRU has no sparsity to exploit, which is the point of comparison */
        for (int r = 0; r < 3 * GRU_H; ++r) {
            float a = gru_b_ih[r];
            const float *w = &gru_w_ih[r * GRU_NF];
            for (int k = 0; k < GRU_NF; ++k) a += w[k] * xn[k];
            gi[r] = a;
            macs += GRU_NF;

            float b = gru_b_hh[r];
            const float *v = &gru_w_hh[r * GRU_H];
            for (int k = 0; k < GRU_H; ++k) b += v[k] * h[k];
            gh[r] = b;
            macs += GRU_H;
        }

        for (int i = 0; i < GRU_H; ++i) {
            float rg = sigmoidf(gi[R_OFF + i] + gh[R_OFF + i]);
            float zg = sigmoidf(gi[Z_OFF + i] + gh[Z_OFF + i]);
            float ng = tanhf(gi[N_OFF + i] + rg * gh[N_OFF + i]);  /* r AFTER b_hn */
            h[i] = (1.0f - zg) * ng + zg * h[i];
            nonlin += 3;
        }
    }

    /* readout from the final hidden state */
    for (int k = 0; k < GRU_NOUT; ++k) {
        float y = gru_out_b[k];
        const float *w = &gru_out_w[k * GRU_H];
        for (int i = 0; i < GRU_H; ++i) y += w[i] * h[i];
        macs += GRU_H;
        out[k] = y * GRU_Y_SIG + GRU_Y_MU;
    }

    if (st) { st->macs = macs; st->nonlin = nonlin; }
}
