#pragma once

// Live inference path for the SpikeSoil node.
//
// Owns the 12-slot sliding window, the fifth-channel computation, the validity
// gate, and the call into the verified SNN kernel. It does NOT normalise: the
// kernel takes raw physical units and applies its own baked-in mu/sd.
//
// logger.* and the CSV schema are untouched, so recordings made by this
// firmware stay readable by the ml/ pipeline and by day-5 replay mode.

#include <stdint.h>
#include <stddef.h>

#include "sensors.h"

// Why a reading is or is not displayable. Anything other than Valid means the
// UI shows the state, not a number.
enum class InferState : uint8_t {
  SelfTestFailed,          // golden-vector check failed at boot; kernel not trusted
  WarmingUp,               // fewer than 12 timesteps buffered since boot
  SensorFault,             // a NaN somewhere in the window
  InsufficientIrradiance,  // lux or iB below the trained range somewhere in the window
  Valid
};

struct InferResult {
  InferState state;
  float loss;       // soiling loss fraction for this window. Valid only if state == Valid.
  float lossEma;    // display-smoothed loss.
  float spikeRate;  // st.rate from the kernel
  unsigned long spikes;
  unsigned long macs;
  uint32_t latencyUs;  // measured kernel time; this is what LAT_EVENT_US needs
  // Newest timestep, so the caller can print why a gate tripped.
  float lux;
  float iB_mA;
};

// Runs the 5-sample golden self-test. Because golden_in is in raw physical
// units, this covers channel order, the milliamp scaling and the kernel's
// internal normalisation as well as its arithmetic. Returns false if any
// sample misses, after which every inferenceRun() returns SelfTestFailed.
bool inferenceInit();

// Call once per timestep (1 Hz). Pushes one timestep, computes the fifth
// channel, slides the window. Pass a value already averaged over the tick —
// training timesteps were 25 s means, not instantaneous samples.
void inferencePush(const SensorReading &r);

// Runs the kernel if the window is full and passes the gate; cheap when gated.
InferResult inferenceRun();

const char *inferStateName(InferState s);
void inferenceFormat(const InferResult &r, char *out, size_t outLen);
