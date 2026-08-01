#include <stdio.h>
#include <math.h>
#include "lif_snn.h"
#include "golden.h"
#include "soiling_snn.h"
int main(void){
    snn_stats_t st, sd;
    float worst = 0.0f;
    printf("  #      C (sparse)        C (dense)          PyTorch        abs err\n");
    for (int n = 0; n < N_GOLDEN; ++n) {
        const float *x = &golden_in[n * SNN_T * SNN_NF];
        float a = snn_infer(x, &st);
        float b = snn_infer_dense(x, &sd);
        float e = fabsf(a - golden_out[n]);
        if (e > worst) worst = e;
        printf("%3d   %14.8f   %14.8f   %14.8f   %10.3e\n", n, a, b, golden_out[n], e);
    }
    printf("\nworst abs error vs PyTorch: %.3e\n", worst);
    printf("sparse: %lu MACs, %lu spikes, rate %.4f\n", st.macs, st.spikes, st.rate);
    printf("dense : %lu MACs\n", sd.macs);
    printf("MAC reduction: %.1f%%\n", 100.0*(1.0 - (double)st.macs/(double)sd.macs));
    return worst < 1e-4f ? 0 : 1;
}
