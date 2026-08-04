#pragma once

// SoftAP dashboard for the SpikeSoil node.
//
// Runs its own access point, so nothing here depends on venue WiFi, an MQTT
// broker, or any external server. A phone joins AP_SSID and opens http://<ip>/.
//
// This module is a pure consumer. It does not read sensors, does not touch the
// SD card, and does not call into the inference kernel -- main.cpp hands it a
// snapshot once per timestep via webuiPublish(). inference.{h,cpp}, logger.*,
// sensors.* and src/bench/ are unmodified.
//
// The server is the core's synchronous WebServer, driven from loop(). That
// keeps everything on one task -- no locking, no second stack -- at the cost of
// blocking the loop while a response is written. The cadence counters below
// measure exactly that cost.

#include <stdint.h>

#include "inference.h"
#include "sensors.h"

// Loop cadence audit, owned and updated by main.cpp, mirrored into /api/state.
//
// The point of exposing this on the dashboard is that the 1 Hz sample cadence
// is a claim the node should be able to defend on the spot: if the async
// server perturbs the loop, these counters say so rather than hiding it.
struct LoopTiming {
  uint32_t ticks;        // 250 ms sub-sample ticks executed since boot
  uint32_t lateTicks;    // ticks that fired >kLateThresholdMs after schedule
  uint32_t maxLateMs;    // worst single tick lateness
  uint64_t sumLateMs;    // for the mean
  uint32_t resyncs;      // long-stall resyncs (main.cpp's >4-tick branch)
  uint32_t steps;        // completed 1 Hz timesteps
  uint32_t minStepMs;    // observed step period extremes; both want 1000
  uint32_t maxStepMs;
};

// Brings up the SoftAP and the HTTP server. Call after any STA work is fully
// torn down -- this switches the radio to WIFI_AP. Returns false if the AP
// failed to start, in which case no server is listening.
bool webuiBegin();

// Services at most one pending HTTP request, inline. Call from loop() on every
// sub-sample tick: a phone polling /api/state once a second should never wait
// on the 1 Hz timestep boundary to be answered.
void webuiHandleClient();

// Call once per completed timestep, immediately after inferenceRun().
//
// Pass the same averaged SensorReading that was pushed into the model, so the
// lux/current shown beside a loss are the ones that produced it. Note that
// main.cpp's accumMean() only averages vB/iB/lux/tB; the panel A channels ride
// through from the most recent sub-sample and are labelled as instantaneous on
// the page.
void webuiPublish(const SensorReading &r, const InferResult &res, const LoopTiming &t);

// Marks timestamps as wall-clock-backed. Purely cosmetic: the page says whether
// the session clock is real or relative to boot.
void webuiSetTimeSynced(bool synced);
