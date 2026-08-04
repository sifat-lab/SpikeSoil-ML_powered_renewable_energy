#include "inference.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Kernel interface — confirmed against src/bench/lif_snn.h and soiling_golden.h.
//
//   void snn_infer(const float *x_raw, float *out, snn_stats_t *st);
//   typedef struct { unsigned long spikes; unsigned long macs; float rate; } snn_stats_t;
//
// x_raw is RAW physical units — volts, milliamps, lux, degC, iB_per_klux —
// flat, [timestep][channel], channel fastest. The kernel applies the mu/sd
// baked into soiling_snn.h itself, so the firmware must NOT normalise.
// (Confirmed by golden_in, whose first five values are 3.92 / 245.7 / 58292.75
// / 31.0 / 4.21 — unmistakably raw physical units.)
// ---------------------------------------------------------------------------
extern "C" {
#include "lif_snn.h"
}

#if defined(SNN_GOLDEN_HEADER)
#include SNN_GOLDEN_HEADER
#else
#error "No SNN_GOLDEN_HEADER: the kernel would run unverified. Add -DSNN_GOLDEN_HEADER to the esp32s3 env."
#endif

#ifndef SNN_T
#define SNN_T 12
#endif
#ifndef SNN_C
#define SNN_C 5
#endif

// golden_in holds N_GOLDEN samples of SNN_T x SNN_C floats. If the geometry
// were wrong the build fails here rather than the self-test walking off the
// end of the array.
static_assert(sizeof(golden_in) / sizeof(golden_in[0]) == N_GOLDEN * SNN_T * SNN_C,
              "golden_in size does not match N_GOLDEN x SNN_T x SNN_C");
static_assert(sizeof(golden_out) / sizeof(golden_out[0]) == N_GOLDEN,
              "golden_out size does not match N_GOLDEN");

namespace {

// Channel order — confirmed twice: ml_05's FE list, and the `features` array
// inside soiling_snn.npz: ['vB', 'iB_mA', 'lux', 'tB_C', 'iB_per_klux'].
enum : uint8_t {
  kChanVB = 0,        // panel B bus voltage, V
  kChanIB = 1,        // panel B current, mA
  kChanLux = 2,       // BH1750 illuminance, lux
  kChanTB = 3,        // panel B surface temperature, degC
  kChanIBperKlux = 4  // 1000 * iB_mA / max(lux, 1)
};

// Validity gate. Training spanned 13.6-76.9 klux and ml_03 discarded anything
// under 5 klux, so below that the model extrapolates and the honest output is
// a state, not a number.
constexpr float kMinLux = 5000.0f;
constexpr float kMinCurrentMa = 20.0f;

// Phase D recorded 1.6e-7 agreement with PyTorch. 1e-4 leaves room for
// float32 accumulation order without hiding a real regression.
constexpr float kSelfTestTol = 1e-4f;

// Display smoothing only; any quoted number comes from the raw per-window value.
constexpr float kEmaAlpha = 0.4f;

float g_ring[SNN_T][SNN_C];
uint8_t g_head = 0;   // index of the OLDEST timestep
uint8_t g_count = 0;  // timesteps buffered since boot, saturating at SNN_T
bool g_kernelTrusted = false;
bool g_emaPrimed = false;
float g_ema = 0.0f;

inline bool finite(float v) { return !isnan(v) && !isinf(v); }

void flattenWindow(float out[SNN_T * SNN_C]) {
  for (uint8_t t = 0; t < SNN_T; ++t) {
    const uint8_t idx = (g_head + t) % SNN_T;
    for (uint8_t c = 0; c < SNN_C; ++c) {
      out[t * SNN_C + c] = g_ring[idx][c];
    }
  }
}

}  // namespace

bool inferenceInit() {
  g_head = 0;
  g_count = 0;
  g_emaPrimed = false;
  g_kernelTrusted = false;

  // golden_in is raw, so this exercises the entire path the live node uses:
  // channel order, the 1000x milliamp scaling, the internal mu/sd, and the
  // kernel arithmetic. All four fail here rather than silently on the roof.
  float worst = 0.0f;
  for (int i = 0; i < N_GOLDEN; ++i) {
    float loss = NAN;
    snn_stats_t st;
    memset(&st, 0, sizeof(st));
    snn_infer(&golden_in[i * SNN_T * SNN_C], &loss, &st);

    const float err = fabsf(loss - golden_out[i]);
    worst = finite(err) ? fmaxf(worst, err) : 1e9f;
    Serial.printf("[snn] golden[%d] got %.7f want %.7f err %.2e  (spikes %lu macs %lu rate %.3f)\n",
                  i, loss, golden_out[i], err, st.spikes, st.macs, st.rate);
  }

  g_kernelTrusted = (worst <= kSelfTestTol);
  Serial.printf("[snn] self-test %s (worst %.2e, tol %.0e)\n",
                g_kernelTrusted ? "PASS" : "FAIL", worst, kSelfTestTol);
  if (!g_kernelTrusted) {
    Serial.println("[snn] inference disabled; suspect channel order, units, or mu/sd");
  }
  return g_kernelTrusted;
}

void inferencePush(const SensorReading &r) {
  const uint8_t slot = (g_head + g_count) % SNN_T;

  g_ring[slot][kChanVB] = r.vB_V;
  g_ring[slot][kChanIB] = r.iB_mA;
  g_ring[slot][kChanLux] = r.lux;
  g_ring[slot][kChanTB] = r.tB_C;

  // Exactly ml_05's fifth channel:
  //     1000 * (X[:,:,1] / np.maximum(X[:,:,2], 1.0))
  const float lux = finite(r.lux) ? fmaxf(r.lux, 1.0f) : NAN;
  g_ring[slot][kChanIBperKlux] =
      (finite(r.iB_mA) && finite(lux)) ? (1000.0f * r.iB_mA / lux) : NAN;

  if (g_count < SNN_T) {
    ++g_count;
  } else {
    g_head = (g_head + 1) % SNN_T;  // full: slide, dropping the oldest
  }
}

InferResult inferenceRun() {
  InferResult out;
  out.state = InferState::WarmingUp;
  out.loss = NAN;
  out.lossEma = NAN;
  out.spikeRate = NAN;
  out.spikes = 0;
  out.macs = 0;
  out.latencyUs = 0;

  const uint8_t newest = (g_head + (g_count ? g_count - 1 : 0)) % SNN_T;
  out.lux = g_count ? g_ring[newest][kChanLux] : NAN;
  out.iB_mA = g_count ? g_ring[newest][kChanIB] : NAN;

  if (!g_kernelTrusted) {
    out.state = InferState::SelfTestFailed;
    return out;
  }
  if (g_count < SNN_T) {
    return out;  // WarmingUp
  }

  // Gate on the whole window, not just the newest timestep: a window that
  // straddles a shading transition contains data the model never saw.
  for (uint8_t t = 0; t < SNN_T; ++t) {
    const uint8_t idx = (g_head + t) % SNN_T;
    for (uint8_t c = 0; c < SNN_C; ++c) {
      if (!finite(g_ring[idx][c])) {
        out.state = InferState::SensorFault;
        g_emaPrimed = false;
        return out;
      }
    }
    if (g_ring[idx][kChanLux] < kMinLux || g_ring[idx][kChanIB] < kMinCurrentMa) {
      out.state = InferState::InsufficientIrradiance;
      g_emaPrimed = false;  // do not carry a stale value across a dark period
      return out;
    }
  }

  float win[SNN_T * SNN_C];
  flattenWindow(win);

  float loss = NAN;
  snn_stats_t st;
  memset(&st, 0, sizeof(st));

  const uint32_t t0 = micros();
  snn_infer(win, &loss, &st);
  out.latencyUs = micros() - t0;

  if (!finite(loss)) {
    out.state = InferState::SensorFault;
    return out;
  }

  g_ema = g_emaPrimed ? (kEmaAlpha * loss + (1.0f - kEmaAlpha) * g_ema) : loss;
  g_emaPrimed = true;

  out.state = InferState::Valid;
  out.loss = loss;
  out.lossEma = g_ema;
  out.spikeRate = st.rate;
  out.spikes = st.spikes;
  out.macs = st.macs;
  return out;
}

const char *inferStateName(InferState s) {
  switch (s) {
    case InferState::SelfTestFailed:         return "kernel self-test failed";
    case InferState::WarmingUp:              return "warming up";
    case InferState::SensorFault:            return "sensor fault";
    case InferState::InsufficientIrradiance: return "insufficient irradiance";
    case InferState::Valid:                  return "ok";
    default:                                 return "unknown";
  }
}

void inferenceFormat(const InferResult &r, char *out, size_t outLen) {
  if (r.state == InferState::Valid) {
    snprintf(out, outLen,
             "[infer] loss=%.3f ema=%.3f rate=%.3f macs=%lu lat=%.2fms lux=%.0f iB=%.0fmA",
             r.loss, r.lossEma, r.spikeRate, r.macs, r.latencyUs / 1000.0f, r.lux,
             r.iB_mA);
  } else if (r.state == InferState::InsufficientIrradiance) {
    snprintf(out, outLen, "[infer] %s (lux=%.0f iB=%.0fmA)", inferStateName(r.state),
             r.lux, r.iB_mA);
  } else {
    snprintf(out, outLen, "[infer] %s", inferStateName(r.state));
  }
}
