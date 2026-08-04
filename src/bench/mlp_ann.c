/* mlp_ann.c -- dense ANN baseline for the soiling task (Phase D)
 *
 * The fair-comparison rule: this kernel is handed the SAME raw 12 x 4 window
 * the SNN kernel receives, and does its own feature extraction. Feeding the
 * MLP pre-computed window statistics would hide that work and make the energy
 * comparison meaningless -- the MLP would start from a position the SNN had to
 * pay for.
 *
 * Both kernels are hand-written float32 C in the same style. Running the ANN
 * through TFLite-Micro instead would have measured the interpreter, not the
 * model, and the SNN would "win" for the wrong reason.
 *
 * Architecture: 4 features -> 8 -> 8 -> 1, tanh activations (scikit-learn
 * MLPRegressor, solver=lbfgs, alpha=0.1). 121 parameters.
 */

#include <math.h>
#include "mlp_ann.h"
#include "soiling_mlp.h"

/* Channel order in the window buffer, matching the SNN's first four channels */
#define CH_VB  0
#define CH_IB  1
#define CH_LUX 2
#define CH_TB  3

float mlp_infer(const float *x_raw, mlp_stats_t *st)
{
    unsigned long macs = 0, tanhs = 0;

    /* ---- feature extraction ---------------------------------------------
     * iB/lux and pB/lux are current and power normalised by irradiance, i.e.
     * measured performance ratios. Two divisions per inference -- on Xtensa
     * a float divide is far slower than a multiply, and this is the ANN's
     * equivalent of the SNN's branch overhead. */
    float vm = 0, im = 0, lm = 0, tm = 0, pm = 0;
    for (int t = 0; t < MLP_T; ++t) {
        const float *r = x_raw + t * MLP_NCH;
        vm += r[CH_VB];
        im += r[CH_IB];
        lm += r[CH_LUX];
        tm += r[CH_TB];
        pm += r[CH_VB] * r[CH_IB] * 0.001f;      /* mW -> same scale as training */
        macs += 1;
    }
    const float inv = 1.0f / (float)MLP_T;
    vm *= inv; im *= inv; lm *= inv; tm *= inv; pm *= inv;
    if (lm < 1.0f) lm = 1.0f;                    /* guard, as in training */

    float f[MLP_NFEAT];
    f[0] = 1000.0f * im / lm;
    f[1] = 1000.0f * pm / lm;
    f[2] = vm;
    f[3] = tm;

    for (int k = 0; k < MLP_NFEAT; ++k)
        f[k] = (f[k] - mlp_mu[k]) / mlp_sd[k];

    /* ---- layer 1: 4 -> 8, tanh ---- */
    float h0[MLP_H0];
    for (int j = 0; j < MLP_H0; ++j) h0[j] = mlp_b0[j];
    for (int i = 0; i < MLP_NFEAT; ++i) {
        const float *w = &mlp_w0[i * MLP_H0];     /* sklearn layout: [in][out] */
        for (int j = 0; j < MLP_H0; ++j) h0[j] += f[i] * w[j];
        macs += MLP_H0;
    }
    for (int j = 0; j < MLP_H0; ++j) { h0[j] = tanhf(h0[j]); tanhs++; }

    /* ---- layer 2: 8 -> 8, tanh ---- */
    float h1[MLP_H1];
    for (int j = 0; j < MLP_H1; ++j) h1[j] = mlp_b1[j];
    for (int i = 0; i < MLP_H0; ++i) {
        const float *w = &mlp_w1[i * MLP_H1];
        for (int j = 0; j < MLP_H1; ++j) h1[j] += h0[i] * w[j];
        macs += MLP_H1;
    }
    for (int j = 0; j < MLP_H1; ++j) { h1[j] = tanhf(h1[j]); tanhs++; }

    /* ---- output: 8 -> 1, identity ---- */
    float y = mlp_b2[0];
    for (int i = 0; i < MLP_H1; ++i) { y += h1[i] * mlp_w2[i]; macs++; }

    if (st) { st->macs = macs; st->tanhs = tanhs; }
    return y;
}
