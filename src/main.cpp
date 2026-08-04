#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "secrets.h"
#include "sensors.h"
#include "logger.h"
#include "inference.h"

namespace {

constexpr uint32_t kWifiTimeoutMs = 15000;
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

constexpr uint32_t kReconnectIntervalMs = 5UL * 60UL * 1000UL;

bool g_timeSynced = false;
uint32_t g_nextSubMs = 0;
uint32_t g_lastReconnectAttemptMs = 0;
uint8_t g_subCount = 0;
uint8_t g_stepsSinceLog = 0;
SensorReading g_lastReading{};
InferResult g_lastResult{};

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

bool connectWifiAndSyncTime() {
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

  configTime(kTzOffsetSec, 0, "pool.ntp.org");
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("[ntp] sync failed; logging with relative timestamps");
    return false;
  }
  Serial.println("[ntp] time synced");
  return true;
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

// A retry blocks for up to 25 s (15 s association + 10 s NTP), which tears a
// hole through two windows. So only retry while inference is already gated --
// at night, indoors, or under a fault. A node that is actively estimating loss
// is never interrupted.
void maybeRetryWifi(uint32_t nowMs) {
  if (g_timeSynced) return;
  if (nowMs - g_lastReconnectAttemptMs < kReconnectIntervalMs) return;
  if (g_lastResult.state == InferState::Valid) return;

  g_lastReconnectAttemptMs = nowMs;
  Serial.println("[wifi] retrying WiFi/NTP sync...");
  if (connectWifiAndSyncTime()) {
    g_timeSynced = true;
    Serial.println("[wifi] time synced mid-run; switching to dated log file");
  }
  g_nextSubMs = millis() + kSubSampleMs;  // resync cadence after the stall
  g_subCount = 0;
  accumReset();
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

  g_timeSynced = connectWifiAndSyncTime();

  g_lastResult.state = InferState::WarmingUp;
  accumReset();
  g_nextSubMs = millis() + kSubSampleMs;
  g_lastReconnectAttemptMs = millis();
}

void loop() {
  const uint32_t nowMs = millis();

  maybeRetryWifi(nowMs);

  if (static_cast<int32_t>(nowMs - g_nextSubMs) < 0) {
    return;
  }
  g_nextSubMs += kSubSampleMs;  // fixed cadence, no accumulated drift
  if (static_cast<int32_t>(nowMs - g_nextSubMs) > static_cast<int32_t>(4 * kSubSampleMs)) {
    g_nextSubMs = nowMs + kSubSampleMs;  // long stall: resync rather than burst
  }

  g_lastReading = sensorsRead();
  accumAdd(g_lastReading);

  if (++g_subCount < kSubSamplesPerStep) {
    return;
  }
  g_subCount = 0;

  const SensorReading step = accumMean();
  accumReset();

  inferencePush(step);
  g_lastResult = inferenceRun();

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
