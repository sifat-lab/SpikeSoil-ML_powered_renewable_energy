#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "secrets.h"
#include "sensors.h"
#include "logger.h"

namespace {

constexpr uint32_t kWifiTimeoutMs = 15000;
constexpr long kTzOffsetSec = 6 * 3600;  // UTC+6 (Dhaka)
constexpr uint32_t kLogIntervalMs = 5000;
constexpr uint32_t kReconnectIntervalMs = 5UL * 60UL * 1000UL;   // retry WiFi+NTP every 5 min
constexpr uint32_t kEpochPersistIntervalMs = 10UL * 60UL * 1000UL;  // save clock floor every 10 min

bool g_timeSynced = false;

// Clock floor restored from SD (or set once NTP syncs) so timestamps stay
// monotonic and roughly correct across a reboot that happens without WiFi.
bool g_haveFloorEpoch = false;
uint32_t g_floorEpoch = 0;
uint32_t g_floorSetMs = 0;

uint32_t g_lastLogMs = 0;
uint32_t g_lastReconnectAttemptMs = 0;
uint32_t g_lastEpochPersistMs = 0;

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

// Best current estimate of the unix epoch: the synced system clock if NTP
// has landed, else the restored/last-known floor advanced by elapsed
// millis(), else 0 (no notion of real time at all yet).
uint32_t currentEstimatedEpoch() {
  if (g_timeSynced) {
    return static_cast<uint32_t>(time(nullptr));
  }
  if (g_haveFloorEpoch) {
    return g_floorEpoch + (millis() - g_floorSetMs) / 1000;
  }
  return 0;
}

// Local (UTC+6) timestamp + daily-file date key. Three cases, in order of
// preference: NTP-synced system clock; a restored/carried-forward floor
// epoch (marked with a leading '~' since it's approximate, but still
// monotonic and roughly correct); or a millis()-based REL+ fallback when we
// have no notion of real time at all.
RowTime currentRowTime() {
  RowTime rt;

  if (g_timeSynced) {
    time_t now = time(nullptr);
    if (now > kTzOffsetSec) {
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);

      char dateBuf[9];
      strftime(dateBuf, sizeof(dateBuf), "%Y%m%d", &timeinfo);

      char isoBuf[24];
      strftime(isoBuf, sizeof(isoBuf), "%Y-%m-%dT%H:%M:%S", &timeinfo);

      rt.dateKey = dateBuf;
      rt.timestamp = String(isoBuf) + "+06:00";
      rt.epoch = static_cast<uint32_t>(now);
      return rt;
    }
  } else if (g_haveFloorEpoch) {
    const uint32_t epoch = g_floorEpoch + (millis() - g_floorSetMs) / 1000;
    const time_t localT = static_cast<time_t>(epoch) + kTzOffsetSec;
    struct tm timeinfo;
    gmtime_r(&localT, &timeinfo);

    char dateBuf[9];
    strftime(dateBuf, sizeof(dateBuf), "%Y%m%d", &timeinfo);

    char isoBuf[24];
    strftime(isoBuf, sizeof(isoBuf), "%Y-%m-%dT%H:%M:%S", &timeinfo);

    rt.dateKey = dateBuf;
    rt.timestamp = "~" + String(isoBuf) + "+06:00";
    rt.epoch = epoch;
    return rt;
  }

  rt.dateKey = "nodate";
  rt.timestamp = "REL+" + String(millis() / 1000.0, 3) + "s";
  rt.epoch = 0;
  return rt;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(3000);

  Serial.printf("Flash: %u bytes, PSRAM: %u bytes\n", ESP.getFlashChipSize(), ESP.getPsramSize());

  sensorsInit();
  loggerInit();

  g_timeSynced = connectWifiAndSyncTime();

  if (!g_timeSynced) {
    uint32_t floorEpoch = 0;
    if (loggerLoadPersistedEpoch(&floorEpoch)) {
      g_haveFloorEpoch = true;
      g_floorEpoch = floorEpoch;
      g_floorSetMs = millis();
      Serial.print("[clock] no NTP yet; restored floor epoch from SD: ");
      Serial.println(floorEpoch);
    }
  }

  const uint32_t nowMs = millis();
  g_lastLogMs = nowMs;
  g_lastReconnectAttemptMs = nowMs;
  g_lastEpochPersistMs = nowMs;
}

void loop() {
  const uint32_t nowMs = millis();

  // Field units may boot or spend most of their life out of WiFi range;
  // keep retrying both WiFi and NTP (not just NTP) since the device can
  // wander back into range mid-run.
  if (!g_timeSynced && nowMs - g_lastReconnectAttemptMs >= kReconnectIntervalMs) {
    g_lastReconnectAttemptMs = nowMs;
    Serial.println("[wifi] retrying WiFi/NTP sync...");
    if (connectWifiAndSyncTime()) {
      g_timeSynced = true;
      g_haveFloorEpoch = false;  // system clock is authoritative now
      Serial.println("[wifi] time synced mid-run; switching to dated log file");
    }
  }

  if (nowMs - g_lastEpochPersistMs >= kEpochPersistIntervalMs) {
    g_lastEpochPersistMs = nowMs;
    const uint32_t epoch = currentEstimatedEpoch();
    if (epoch != 0) {
      loggerPersistEpoch(epoch);
    }
  }

  if (nowMs - g_lastLogMs < kLogIntervalMs) {
    return;
  }
  g_lastLogMs = nowMs;

  const RowTime rt = currentRowTime();
  const SensorReading reading = sensorsRead();

  String csvRow;
  loggerWriteRow(rt.dateKey, rt.timestamp, rt.epoch, reading, &csvRow);

  Serial.println(csvRow);
}
