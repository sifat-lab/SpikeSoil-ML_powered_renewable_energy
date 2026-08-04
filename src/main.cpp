#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "secrets.h"
#include "sensors.h"
#include "logger.h"
#include "inference.h"
#include "webui.h"

// Set to 0 to build the identical firmware without the SoftAP or HTTP server.
// The cadence counters below are compiled either way, so the same numbers can
// be read off both builds and compared honestly.
#ifndef WEBUI_ENABLED
#define WEBUI_ENABLED 1
#endif

namespace {

constexpr uint32_t kWifiTimeoutMs = 15000;

// Per-attempt, and there are two attempts. 8 s each keeps the worst-case boot
// at 15 + 16 = 31 s, against 25 s for the single 10 s attempt it replaces.
constexpr uint32_t kNtpTimeoutMs = 8000;
constexpr long kTzOffsetSec = 6 * 3600;  // UTC+6 (Dhaka)

// Sensors are read at 4 Hz and averaged into one timestep per second.
//
// Why average: ml_05 built each timestep as a 25 s mean of the 5 s log rows,
// i.e. roughly five readings averaged. mu/sd in soiling_snn.npz describe that
// smoothed distribution (lux sd = 20761, iB sd = 71.5 mA). Feeding single
// instantaneous readings would present the kernel with a noisier signal than
// anything it was normalised against. Four sub-samples recover most of it at
// no meaningful cost. Set kSubSamplesPerStep = 1 to disable.
constexpr uint32_t kSubSampleMs = 250;
constexpr uint8_t kSubSamplesPerStep = 4;   // -> one timestep per second
constexpr uint8_t kStepsPerLog = 5;         // -> one SD row every 5 s, unchanged

// A tick counts as late past this. 50 ms is a fifth of the sub-sample period
// and still an order of magnitude below the 1 Hz timestep it feeds.
constexpr uint32_t kLateThresholdMs = 50;

bool g_timeSynced = false;
uint32_t g_nextSubMs = 0;
uint8_t g_subCount = 0;
uint8_t g_stepsSinceLog = 0;
SensorReading g_lastReading{};
InferResult g_lastResult{};

LoopTiming g_timing{};
uint32_t g_lastStepMs = 0;

// Per-channel accumulator: a channel that reads NAN on some sub-samples still
// averages the ones that succeeded, and only yields NAN if all of them failed.
struct Accum {
  float sum[4];
  uint8_t n[4];
};
Accum g_acc{};

inline bool finiteVal(float v) { return !isnan(v) && !isinf(v); }

void accumReset() {
  for (uint8_t i = 0; i < 4; ++i) {
    g_acc.sum[i] = 0.0f;
    g_acc.n[i] = 0;
  }
}

void accumAdd(const SensorReading &r) {
  const float v[4] = {r.vB_V, r.iB_mA, r.lux, r.tB_C};
  for (uint8_t i = 0; i < 4; ++i) {
    if (finiteVal(v[i])) {
      g_acc.sum[i] += v[i];
      g_acc.n[i] += 1;
    }
  }
}

SensorReading accumMean() {
  float m[4];
  for (uint8_t i = 0; i < 4; ++i) {
    m[i] = g_acc.n[i] ? (g_acc.sum[i] / g_acc.n[i]) : NAN;
  }
  SensorReading r = g_lastReading;  // channels the model ignores pass through
  r.vB_V = m[0];
  r.iB_mA = m[1];
  r.lux = m[2];
  r.tB_C = m[3];
  return r;
}

struct RowTime {
  String dateKey;
  String timestamp;
  uint32_t epoch;
};

const char *wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:     return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL:   return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "WL_SCAN_COMPLETED";
    case WL_CONNECTED:       return "WL_CONNECTED";
    case WL_CONNECT_FAILED:  return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED:    return "WL_DISCONNECTED";
    default:                 return "WL_UNKNOWN";
  }
}

bool staConnectAndSyncTime() {
  // Auto-reconnect makes the driver retry association on its own after a
  // failed attempt, which is what produced the "Set status to INIT" flood --
  // we do our own bounded wait below, so it must stay off. Disconnecting
  // any stale STA state before begin() avoids the driver re-entering INIT
  // repeatedly from a dirty previous session.
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  // WiFi.mode()/WiFi.begin() are each called exactly once; the wait below
  // only polls WiFi.status(), it never re-issues begin().
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("[wifi] connecting");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < kWifiTimeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    wl_status_t status = WiFi.status();
    Serial.print("[wifi] connect failed, status=");
    Serial.print(static_cast<int>(status));
    Serial.print(" (");
    Serial.print(wifiStatusName(status));
    Serial.println("); logging with relative timestamps");
    return false;
  }
  Serial.print("[wifi] connected, IP=");
  Serial.println(WiFi.localIP());

  // Two attempts, because there is no second chance: with no mid-session STA
  // retry, one lost UDP exchange here costs the whole session its dated log
  // filenames and ISO timestamps. Observed happening on a real boot.
  //
  // The retry points at a different pool as well as re-issuing the request --
  // the common failure is one unlucky server or a slow first DNS resolution,
  // not the venue link being unusable, and the link is known good by this
  // point because association already succeeded.
  struct tm timeinfo;

  configTime(kTzOffsetSec, 0, "pool.ntp.org");
  if (getLocalTime(&timeinfo, kNtpTimeoutMs)) {
    Serial.println("[ntp] time synced");
    return true;
  }

  Serial.println("[ntp] first attempt failed; retrying once before the AP comes up");
  configTime(kTzOffsetSec, 0, "time.google.com", "pool.ntp.org");
  if (getLocalTime(&timeinfo, kNtpTimeoutMs)) {
    Serial.println("[ntp] time synced on retry");
    return true;
  }

  Serial.println("[ntp] sync failed twice; logging with relative timestamps");
  return false;
}

// One-shot wall-clock acquisition, then the STA interface is destroyed for
// good.
//
// The node's own AP is the deliverable, and SoftAP + STA share one radio and
// therefore one channel: in WIFI_AP_STA the STA leg dictates the channel, so
// associating to the venue AP drags the SoftAP onto that channel and drops
// every connected phone, and a *failed* association leaves the STA scanning,
// which is off-channel and makes the AP intermittently deaf. Neither is
// acceptable in front of a panel.
//
// Running STA to completion first and tearing it down before the AP exists
// avoids all of that -- the two modes are never live at the same instant --
// while still giving the SD log real dated filenames for the whole session,
// since the RTC keeps running afterwards. If the venue WiFi is absent this
// costs the same 15 s it always did and falls back to relative timestamps.
//
// This is still the only STA window in the session, and it stays that way: a
// mid-session retry would have to re-enter STA, which drags the SoftAP onto the
// venue's channel and disconnects every phone watching the dashboard. The
// retry added inside staConnectAndSyncTime() is therefore a second NTP attempt
// on the already-open link, not a second association.
bool syncTimeThenReleaseRadio() {
  const bool ok = staConnectAndSyncTime();

  WiFi.disconnect(true, true);  // drop association, erase stored config
  WiFi.mode(WIFI_OFF);
  delay(100);
  Serial.printf("[wifi] STA released; timestamps are %s\n",
                ok ? "wall-clock" : "relative to boot");
  return ok;
}

// Local (UTC+6) timestamp + daily-file date key, or a millis()-based
// fallback (epoch=0) when NTP time was never obtained.
RowTime currentRowTime() {
  if (g_timeSynced) {
    time_t now = time(nullptr);
    if (now > kTzOffsetSec) {
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);

      char dateBuf[9];
      strftime(dateBuf, sizeof(dateBuf), "%Y%m%d", &timeinfo);

      char isoBuf[24];
      strftime(isoBuf, sizeof(isoBuf), "%Y-%m-%dT%H:%M:%S", &timeinfo);

      RowTime rt;
      rt.dateKey = dateBuf;
      rt.timestamp = String(isoBuf) + "+06:00";
      rt.epoch = static_cast<uint32_t>(now);
      return rt;
    }
  }

  RowTime rt;
  rt.dateKey = "nodate";
  rt.timestamp = "REL+" + String(millis() / 1000.0, 3) + "s";
  rt.epoch = 0;
  return rt;
}

// The periodic NTP retry is gone with the STA path. It cannot run any more:
// re-entering STA mid-session would move the SoftAP off its channel and
// disconnect every phone watching the dashboard. Time is acquired once at boot
// or not at all.

// Cadence audit. Lateness is measured against the tick's scheduled slot, not
// against the previous tick, so a late tick that is followed by an on-time one
// is not counted twice.
void noteTick(uint32_t nowMs, uint32_t scheduledMs) {
  const uint32_t lateMs = nowMs - scheduledMs;  // >= 0 by the caller's guard
  g_timing.ticks += 1;
  g_timing.sumLateMs += lateMs;
  if (lateMs > g_timing.maxLateMs) g_timing.maxLateMs = lateMs;
  if (lateMs > kLateThresholdMs) g_timing.lateTicks += 1;
}

void noteStep(uint32_t nowMs) {
  if (g_timing.steps > 0) {
    const uint32_t periodMs = nowMs - g_lastStepMs;
    if (periodMs < g_timing.minStepMs || g_timing.minStepMs == 0) {
      g_timing.minStepMs = periodMs;
    }
    if (periodMs > g_timing.maxStepMs) g_timing.maxStepMs = periodMs;
  }
  g_lastStepMs = nowMs;
  g_timing.steps += 1;
}

// The 1 Hz timestep, lifted out of loop() unchanged so that loop() has a single
// exit path and the HTTP service call can sit at the end of every tick without
// being duplicated across two returns.
void runTimestep() {
  const SensorReading step = accumMean();
  accumReset();

  inferencePush(step);
  g_lastResult = inferenceRun();
  noteStep(millis());

  // The dashboard is a pure consumer: it gets the same averaged reading the
  // model saw plus the result, and does no sensor or SD work of its own.
#if WEBUI_ENABLED
  webuiPublish(step, g_lastResult, g_timing);
#endif

  // The CSV keeps carrying instantaneous readings on the same 5 s cadence, so
  // rows written by this firmware are the same kind of data ml_03 was built on.
  if (++g_stepsSinceLog >= kStepsPerLog) {
    g_stepsSinceLog = 0;
    const RowTime rt = currentRowTime();
    String csvRow;
    loggerWriteRow(rt.dateKey, rt.timestamp, rt.epoch, g_lastReading, &csvRow);
    Serial.println(csvRow);
  }

  char line[176];
  inferenceFormat(g_lastResult, line, sizeof(line));
  Serial.println(line);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(3000);

  Serial.printf("Flash: %u bytes, PSRAM: %u bytes\n", ESP.getFlashChipSize(), ESP.getPsramSize());

  sensorsInit();
  loggerInit();

  if (!inferenceInit()) {
    Serial.println("[main] continuing as logger only; no loss estimate will be shown");
  }

  // Order matters: the STA interface must be completely gone before the AP
  // comes up, so the two are never simultaneously live on the shared radio.
  g_timeSynced = syncTimeThenReleaseRadio();

#if WEBUI_ENABLED
  if (!webuiBegin()) {
    Serial.println("[main] continuing headless; logging and inference unaffected");
  }
  webuiSetTimeSynced(g_timeSynced);
#else
  Serial.println("[main] built with WEBUI_ENABLED=0; no AP, no server");
#endif

  g_lastResult.state = InferState::WarmingUp;
  accumReset();
  g_timing = LoopTiming{};
  g_nextSubMs = millis() + kSubSampleMs;
}

void loop() {
  const uint32_t nowMs = millis();

  if (static_cast<int32_t>(nowMs - g_nextSubMs) < 0) {
    return;
  }
  const uint32_t scheduledMs = g_nextSubMs;
  noteTick(nowMs, scheduledMs);

  g_nextSubMs += kSubSampleMs;  // fixed cadence, no accumulated drift
  if (static_cast<int32_t>(nowMs - g_nextSubMs) > static_cast<int32_t>(4 * kSubSampleMs)) {
    g_nextSubMs = nowMs + kSubSampleMs;  // long stall: resync rather than burst
    g_timing.resyncs += 1;
  }

  g_lastReading = sensorsRead();
  accumAdd(g_lastReading);

  if (++g_subCount >= kSubSamplesPerStep) {
    g_subCount = 0;
    runTimestep();
  }

  // Serve HTTP once per sub-sample tick, at the point of maximum slack: the
  // tick's own work is finished and the next tick is ~250 ms away, so a
  // response that takes tens of milliseconds still lands clear of it.
  //
  // This is synchronous -- it blocks the loop for the length of the response.
  // That is why it sits here and not before sensorsRead(), and the cost is
  // reported in LoopTiming rather than hidden.
#if WEBUI_ENABLED
  webuiHandleClient();
#endif
}
