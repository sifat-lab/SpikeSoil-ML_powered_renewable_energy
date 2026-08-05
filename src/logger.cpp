#include "logger.h"

#include <SPI.h>
#include <SdFat.h>
#include <math.h>

namespace {

constexpr int kCsPin = 7;
constexpr int kMosiPin = 6;
constexpr int kSckPin = 4;
constexpr int kMisoPin = 5;
constexpr int kLedPin = 38;  // onboard RGB LED (WS2812, driven via neopixelWrite)

const char kCsvHeader[] =
    "timestamp,epoch,vA,iA_mA,pA_mW,vB,iB_mA,pB_mW,lux,tA_C,tB_C";

constexpr uint32_t kSdRetryIntervalMs = 30000;

SPIClass spi(FSPI);
SdFat sd;
bool sdReady = false;
uint32_t lastSdRetryMs = 0;

// The single point of contact with sd.begin(). lastSdRetryMs is seeded by
// loggerInit()'s initial attempt (success or failure), so this cooldown
// applies from boot onward -- there is no "never retried yet" bypass that
// would let a second caller fire a begin() back-to-back with the first and
// wedge the card's SPI init sequence.
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

// Pure accessor: it adds a reader to the existing card handle and changes
// nothing about how rows are written. See the note in logger.h for why replay
// borrows this instead of mounting the card a second time.
SdFat *loggerSdCard() { return ensureSdReady() ? &sd : nullptr; }
