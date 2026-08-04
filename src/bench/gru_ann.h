#ifndef GRU_ANN_H
#define GRU_ANN_H
typedef struct { unsigned long macs; unsigned long nonlin; } gru_stats_t;
void gru_infer(const float *x_raw, float *out, gru_stats_t *st);
#endif
