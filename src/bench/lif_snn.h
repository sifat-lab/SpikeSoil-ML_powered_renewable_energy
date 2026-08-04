#ifdef __cplusplus
extern "C" {
#endif
#ifndef LIF_SNN_H
#define LIF_SNN_H
typedef struct { unsigned long spikes; unsigned long macs; float rate; } snn_stats_t;
void  snn_infer(const float *x_raw, float *out, snn_stats_t *st);       /* event-driven */
void  snn_infer_dense(const float *x_raw, float *out, snn_stats_t *st); /* dense reference */
float snn_infer_lif_only(const float *x_raw, snn_stats_t *st); /* neuron-only floor */
#endif
#ifdef __cplusplus
}
#endif