#include "logger.h"

#include <SPI.h>
#include <SD.h>
#include <math.h>

namespace {

constexpr int kCsPin = 10;
constexpr int kMosiPin = 11;
constexpr int kSckPin = 12;
constexpr int kMisoPin = 13;
constexpr int kLedPin = 38;  // onboard RGB LED (WS2812, driven via neopixelWrite)

const char kCsvHeader[] =
    "timestamp,epoch,vA,iA_mA,pA_mW,vB,iB_mA,pB_mW,lux,tA_C,tB_C";

bool sdReady = false;

bool ensureSdReady() {
  if (sdReady) {
    return true;
  }
  sdReady = SD.begin(kCsPin, SPI, 4000000);
  return sdReady;
}

String csvField(float v) {
  if (isnan(v)) {
    return String();
  }
  return String(v, 3);
}

void loggerBlinkFail() {
  neopixelWrite(kLedPin, 40, 0, 0);
  delay(80);
  neopixelWrite(kLedPin, 0, 0, 0);
}

String buildCsvRow(const String &timestamp, uint32_t epoch, const SensorReading &r) {
  String row = timestamp + "," + String(epoch) + "," +
               csvField(r.vA_V) + "," + csvField(r.iA_mA) + "," + csvField(r.pA_mW) + "," +
               csvField(r.vB_V) + "," + csvField(r.iB_mA) + "," + csvField(r.pB_mW) + "," +
               csvField(r.lux) + "," +
               csvField(r.tA_C) + "," + csvField(r.tB_C);
  return row;
}

}  // namespace

void loggerInit() {
  pinMode(kLedPin, OUTPUT);
  neopixelWrite(kLedPin, 0, 0, 0);

  SPI.begin(kSckPin, kMisoPin, kMosiPin, kCsPin);
  sdReady = SD.begin(kCsPin, SPI, 4000000);
  if (!sdReady) {
    Serial.println("[logger] SD card init failed; will retry on next write");
  }
}

bool loggerWriteRow(const String &dateKey, const String &timestamp, uint32_t epoch, const SensorReading &r,
                     String *csvRowOut) {
  const String row = buildCsvRow(timestamp, epoch, r);
  if (csvRowOut != nullptr) {
    *csvRowOut = row;
  }

  if (!ensureSdReady()) {
    loggerBlinkFail();
    return false;
  }

  const String path = "/log_" + dateKey + ".csv";
  const bool isNewFile = !SD.exists(path);

  File f = SD.open(path, FILE_APPEND);
  if (!f) {
    sdReady = false;  // force a re-init attempt on the next cycle
    loggerBlinkFail();
    return false;
  }

  if (isNewFile) {
    f.println(kCsvHeader);
  }
  f.println(row);
  f.close();
  return true;
}
