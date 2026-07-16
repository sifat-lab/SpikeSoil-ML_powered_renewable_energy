#pragma once

#include <Arduino.h>
#include "sensors.h"

void loggerInit();

// Appends one CSV row to /log_<dateKey>.csv on the SD card, writing the
// header first if the file is new. dateKey is typically "YYYYMMDD"; a
// fallback key is used when local time hasn't been synced yet. Returns
// false (and blinks the onboard RGB LED) if the write could not complete;
// the caller should just try again on the next cycle. If csvRowOut is
// non-null, it receives the formatted row regardless of write success,
// so the caller can mirror it to Serial.
bool loggerWriteRow(const String &dateKey, const String &timestamp, uint32_t epoch, const SensorReading &r,
                     String *csvRowOut = nullptr);
