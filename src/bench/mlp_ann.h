#ifndef MLP_ANN_H
#define MLP_ANN_H
typedef struct { unsigned long macs; unsigned long tanhs; } mlp_stats_t;
float mlp_infer(const float *x_raw, mlp_stats_t *st);
#endif
