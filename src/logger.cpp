#include "logger.h"

#include <SPI.h>
#include <SdFat.h>
#include <math.h>
#include <stdlib.h>

namespace {

constexpr int kCsPin = 7;
constexpr int kMosiPin = 6;
constexpr int kSckPin = 4;
constexpr int kMisoPin = 5;
constexpr int kLedPin = 38;  // onboard RGB LED (WS2812, driven via neopixelWrite)

const char kCsvHeader[] =
    "timestamp,epoch,vA,iA_mA,pA_mW,vB,iB_mA,pB_mW,lux,tA_C,tB_C";

const char kEpochStatePath[] = "/clockstate.txt";

constexpr uint32_t kSdRetryIntervalMs = 30000;

SPIClass spi(FSPI);
SdFat sd;
bool sdReady = false;
uint32_t lastSdRetryMs = 0;

// The single point of contact with sd.begin(). lastSdRetryMs is seeded by
// loggerInit()'s initial attempt (success or failure), so this cooldown
// applies from boot onward -- there is no "never retried yet" bypass that
// would let a second caller (e.g. loggerLoadPersistedEpoch) fire a begin()
// back-to-back with the first and wedge the card's SPI init sequence.
bool ensureSdReady() {
  if (sdReady) {
    return true;
  }
  const uint32_t nowMs = millis();
  if (nowMs - lastSdRetryMs < kSdRetryIntervalMs) {
    return false;
  }
  lastSdRetryMs = nowMs;
  Serial.println("[logger][sd.begin] retry attempt...");
  sdReady = sd.begin(SdSpiConfig(kCsPin, SHARED_SPI, SD_SCK_MHZ(4), &spi));
  if (!sdReady) {
    Serial.print("[logger][sd.begin] failed: ");
    printSdErrorSymbol(&Serial, sd.sdErrorCode());
    Serial.printf(" code=0x%02X data=0x%02X\n",
                  sd.sdErrorCode(), sd.sdErrorData());
  } else {
    Serial.println("[logger][sd.begin] ok (retry)");
  }
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

  spi.begin(kSckPin, kMisoPin, kMosiPin, kCsPin);
  sdReady = sd.begin(SdSpiConfig(kCsPin, SHARED_SPI, SD_SCK_MHZ(4), &spi));
  lastSdRetryMs = millis();  // seed the cooldown so a second caller can't retry back-to-back
  if (!sdReady) {
    Serial.println("[logger][sd.begin] failed at boot; will retry on next write");
    Serial.print("[logger][sd.begin] error: ");
    printSdErrorSymbol(&Serial, sd.sdErrorCode());
    Serial.printf(" code=0x%02X data=0x%02X\n",
                  sd.sdErrorCode(), sd.sdErrorData());
  } else {
    Serial.println("[logger][sd.begin] ok");
  }
}

bool loggerWriteRow(const String &dateKey, const String &timestamp, uint32_t epoch, const SensorReading &r,
                     String *csvRowOut) {
  const String row = buildCsvRow(timestamp, epoch, r);
  if (csvRowOut != nullptr) {
    *csvRowOut = row;
  }

  const String path = "/log_" + dateKey + ".csv";

  const bool wasReady = sdReady;
  if (!ensureSdReady()) {
    loggerBlinkFail();
    return false;
  }
  if (!wasReady) {
    Serial.print("[logger] SD recovered, logging to ");
    Serial.println(path);
  }

  const bool isNewFile = !sd.exists(path.c_str());

  FsFile f = sd.open(path.c_str(), O_WRITE | O_CREAT | O_APPEND);
  if (!f) {
    Serial.print("[logger][logfile.open] failed: ");
    Serial.println(path);
    sdReady = false;  // force a re-init attempt on the next cycle
    loggerBlinkFail();
    return false;
  }

  if (isNewFile) {
    f.println(kCsvHeader);
  }
  f.println(row);
  f.sync();
  f.close();
  return true;
}

bool loggerLoadPersistedEpoch(uint32_t *epochOut) {
  // Read-only, boot-time, best-effort: never forces an sd.begin() of its
  // own. If loggerInit()'s mount hasn't succeeded (or its retry cooldown
  // hasn't elapsed), just skip -- ensureSdReady()'s single retry flow
  // (used by loggerWriteRow/loggerPersistEpoch) will bring the card up on
  // its own schedule, without a second caller racing it at boot.
  if (!sdReady) {
    Serial.println("[logger][clockstate.open] SD not ready yet; skipping");
    return false;
  }
  if (!sd.exists(kEpochStatePath)) {
    Serial.println("[logger][clockstate.open] no persisted clock state (first boot)");
    return false;
  }
  FsFile f = sd.open(kEpochStatePath, O_READ);
  if (!f) {
    Serial.println("[logger][clockstate.open] open failed");
    return false;
  }
  char buf[16] = {0};
  const int n = f.read(buf, sizeof(buf) - 1);
  f.close();
  if (n <= 0) {
    Serial.println("[logger][clockstate.open] read failed/empty");
    return false;
  }
  const uint32_t epoch = strtoul(buf, nullptr, 10);
  if (epoch == 0) {
    Serial.println("[logger][clockstate.open] invalid contents");
    return false;
  }
  Serial.println("[logger][clockstate.open] ok");
  *epochOut = epoch;
  return true;
}

void loggerPersistEpoch(uint32_t epoch) {
  // Called only every ~10 minutes from loop(), long past the SD retry
  // cooldown, so going through the shared ensureSdReady() flow here can't
  // race a fresh mount attempt the way a boot-time caller could.
  if (!ensureSdReady()) {
    return;
  }
  FsFile f = sd.open(kEpochStatePath, O_WRITE | O_CREAT | O_TRUNC);
  if (!f) {
    Serial.println("[logger][clockstate.write] open failed");
    return;
  }
  f.print(epoch);
  f.sync();
  f.close();
}
