#include <stdio.h>
#include <math.h>
#include "lif_snn.h"
#include SNN_MODEL_HEADER
#include SNN_GOLDEN_HEADER

int main(void){
    snn_stats_t st, sd;
    float worst = 0.0f, ys[SNN_NOUT], yd[SNN_NOUT];
    unsigned long mac_sum = 0;
    printf("model %d-%d-%d-%d  T=%d  beta=%g  beta_out=%g\n",
           SNN_NF, SNN_H, SNN_H, SNN_NOUT, SNN_T, SNN_BETA, SNN_BETA_OUT);
    for (int n = 0; n < N_GOLDEN; ++n) {
        const float *x = &golden_in[n * SNN_T * SNN_NF];
        snn_infer(x, ys, &st);
        snn_infer_dense(x, yd, &sd);
        mac_sum += st.macs;
        for (int k = 0; k < SNN_NOUT; ++k) {
            float ref = golden_out[n * SNN_NOUT + k];
            float e1 = fabsf(ys[k] - ref), e2 = fabsf(yd[k] - ref);
            if (e1 > worst) worst = e1;
            if (e2 > worst) worst = e2;
        }
        printf("%2d  MACs=%5lu rate=%.4f  out:", n, st.macs, st.rate);
        for (int k = 0; k < SNN_NOUT; ++k) printf(" %9.6f", ys[k]);
        printf("   ref:");
        for (int k = 0; k < SNN_NOUT; ++k) printf(" %9.6f", golden_out[n*SNN_NOUT+k]);
        printf("\n");
    }
    printf("\nworst abs error vs PyTorch: %.3e\n", worst);
    printf("mean MACs %.0f vs dense %lu -> reduction %.1f%%\n",
           (double)mac_sum/N_GOLDEN, sd.macs,
           100.0*(1.0-((double)mac_sum/N_GOLDEN)/sd.macs));
    return worst < 1e-4f ? 0 : 1;
}
