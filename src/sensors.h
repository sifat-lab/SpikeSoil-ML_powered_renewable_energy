#pragma once

// Reads the two INA219 power monitors, the BH1750 light sensor, and the two
// DS18B20 temperature probes. Any field that fails to read is set to NAN so
// the logger can emit an empty CSV cell without dropping the whole row.

struct SensorReading {
  float vA_V;
  float iA_mA;
  float pA_mW;
  float vB_V;
  float iB_mA;
  float pB_mW;
  float lux;
  float tA_C;
  float tB_C;
};

void sensorsInit();
SensorReading sensorsRead();
