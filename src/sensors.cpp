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
constexpr int kOneWirePin = 4;

Adafruit_INA219 ina219A(kIna219AddrA);
Adafruit_INA219 ina219B(kIna219AddrB);
BH1750 lightMeter(kBh1750Addr);
OneWire oneWire(kOneWirePin);
DallasTemperature dsSensors(&oneWire);

bool i2cDevicePresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

}  // namespace

void sensorsInit() {
  Wire.begin(kSdaPin, kSclPin);
  Wire.setClock(kI2cClockHz);

  ina219A.begin(&Wire);
  ina219B.begin(&Wire);
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, kBh1750Addr, &Wire);

  dsSensors.setOneWire(&oneWire);
  dsSensors.begin();
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

  dsSensors.requestTemperatures();
  float tA = dsSensors.getTempCByIndex(0);
  float tB = dsSensors.getTempCByIndex(1);
  if (tA != DEVICE_DISCONNECTED_C) {
    r.tA_C = tA;
  }
  if (tB != DEVICE_DISCONNECTED_C) {
    r.tB_C = tB;
  }

  return r;
}
