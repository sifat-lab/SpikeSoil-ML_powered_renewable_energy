#include "sensors.h"

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <BH1750.h>
#include <OneWire.h>
#include <DallasTemperature.h>

namespace {

constexpr int kSdaPin = 8;
constexpr int kSclPin = 9;
constexpr uint32_t kI2cClockHz = 100000;

constexpr uint8_t kIna219AddrA = 0x40;
constexpr uint8_t kIna219AddrB = 0x41;
constexpr uint8_t kBh1750Addr = 0x23;
constexpr int kOneWirePin = 15;

// At 12-bit the DS18B20 needs 750 ms per conversion and DallasTemperature
// blocks for all of it by default. That was harmless at one row per 5 s; at
// 1 Hz it eats three quarters of the sample period and makes the window
// cadence jitter. So: 11-bit (0.125 degC, 375 ms) and setWaitForConversion
// (false), reading the value requested on the previous tick. The reported
// temperature is therefore up to one sample old -- irrelevant against a panel
// whose thermal time constant is minutes, but it is a deviation, so it is
// written down here and in INTEGRATION_NOTES.md.
constexpr uint8_t kDsResolutionBits = 11;
constexpr uint32_t kDsConversionMs = 400;  // 375 ms plus margin

Adafruit_INA219 ina219A(kIna219AddrA);
Adafruit_INA219 ina219B(kIna219AddrB);
BH1750 lightMeter(kBh1750Addr);
OneWire oneWire(kOneWirePin);
DallasTemperature dsSensors(&oneWire);

uint32_t g_dsRequestMs = 0;
float g_tA_C = NAN;
float g_tB_C = NAN;

bool i2cDevicePresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// Latch the finished conversion (if any) and immediately start the next one.
void serviceTemperatures() {
  if (millis() - g_dsRequestMs < kDsConversionMs) {
    return;  // still converting; keep the previous values
  }

  const float tA = dsSensors.getTempCByIndex(0);
  const float tB = dsSensors.getTempCByIndex(1);
  g_tA_C = (tA != DEVICE_DISCONNECTED_C) ? tA : NAN;
  g_tB_C = (tB != DEVICE_DISCONNECTED_C) ? tB : NAN;

  dsSensors.requestTemperatures();
  g_dsRequestMs = millis();
}

}  // namespace

void sensorsInit() {
  Wire.begin(kSdaPin, kSclPin);
  Wire.setClock(kI2cClockHz);

  ina219A.begin(&Wire);
  ina219B.begin(&Wire);
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, kBh1750Addr, &Wire);
  lightMeter.setMTreg(31);   // lowest sensitivity → range ~121k lux, দুপুরের রোদেও saturate করবে না

  dsSensors.setOneWire(&oneWire);
  dsSensors.begin();
  dsSensors.setResolution(kDsResolutionBits);
  dsSensors.setWaitForConversion(false);

  // Kick off the first conversion and block once, so the first SensorReading
  // already carries a real temperature rather than a NAN.
  dsSensors.requestTemperatures();
  g_dsRequestMs = millis();
  delay(kDsConversionMs);
  serviceTemperatures();
}

SensorReading sensorsRead() {
  SensorReading r;
  r.vA_V = r.iA_mA = r.pA_mW = NAN;
  r.vB_V = r.iB_mA = r.pB_mW = NAN;
  r.lux = NAN;
  r.tA_C = r.tB_C = NAN;

  if (i2cDevicePresent(kIna219AddrA)) {
    r.vA_V = ina219A.getBusVoltage_V();
    r.iA_mA = ina219A.getCurrent_mA();
    r.pA_mW = ina219A.getPower_mW();
  }

  if (i2cDevicePresent(kIna219AddrB)) {
    r.vB_V = ina219B.getBusVoltage_V();
    r.iB_mA = ina219B.getCurrent_mA();
    r.pB_mW = ina219B.getPower_mW();
  }

  if (i2cDevicePresent(kBh1750Addr)) {
    float lux = lightMeter.readLightLevel();
    if (lux >= 0.0f) {
      r.lux = lux;
    }
  }

  serviceTemperatures();
  r.tA_C = g_tA_C;
  r.tB_C = g_tB_C;

  return r;
}
