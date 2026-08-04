#include <stdio.h>
#include <math.h>
#include "mlp_ann.h"
#include "soiling_mlp.h"
#include "soiling_mlp_golden.h"
int main(void){
    mlp_stats_t st; float worst=0;
    printf("MLP %d-%d-%d-1\n", MLP_NFEAT, MLP_H0, MLP_H1);
    for (int n=0;n<N_MLP_GOLDEN;++n){
        float y = mlp_infer(&mlp_golden_in[n*MLP_T*MLP_NCH], &st);
        float e = fabsf(y - mlp_golden_out[n]);
        if (e>worst) worst=e;
        printf("%2d  %10.6f  ref %10.6f   err %.3e   MACs=%lu tanh=%lu\n",
               n, y, mlp_golden_out[n], e, st.macs, st.tanhs);
    }
    printf("\nworst abs error vs scikit-learn: %.3e\n", worst);
    return worst < 1e-4f ? 0 : 1;
}
