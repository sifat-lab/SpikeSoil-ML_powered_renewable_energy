#include <stdio.h>
#include <math.h>
#include "gru_ann.h"
#include "forecast_gru.h"
#include "forecast_gru_golden.h"
int main(void){
    gru_stats_t st; float y[GRU_NOUT], worst=0;
    printf("GRU(%d,%d)+Linear(%d,%d) T=%d\n",GRU_NF,GRU_H,GRU_H,GRU_NOUT,GRU_T);
    for(int n=0;n<N_GRU_GOLDEN;++n){
        gru_infer(&gru_golden_in[n*GRU_T*GRU_NF], y, &st);
        printf("%2d ",n);
        for(int k=0;k<GRU_NOUT;++k){
            float ref=gru_golden_out[n*GRU_NOUT+k], e=fabsf(y[k]-ref);
            if(e>worst) worst=e;
            printf(" %9.6f/%9.6f",y[k],ref);
        }
        printf("   MACs=%lu nonlin=%lu\n",st.macs,st.nonlin);
    }
    printf("\nworst abs error vs PyTorch: %.3e\n",worst);
    return worst<1e-4f?0:1;
}
