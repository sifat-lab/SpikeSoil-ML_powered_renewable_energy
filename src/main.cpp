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

bool g_timeSynced = false;
uint32_t g_lastLogMs = 0;

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

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(3000);

  Serial.printf("Flash: %u bytes, PSRAM: %u bytes\n", ESP.getFlashChipSize(), ESP.getPsramSize());

  sensorsInit();
  loggerInit();

  g_timeSynced = connectWifiAndSyncTime();

  g_lastLogMs = millis();
}

void loop() {
  uint32_t nowMs = millis();
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
