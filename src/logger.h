#pragma once

#include <Arduino.h>
#include <SdFat.h>

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

// Borrowed handle to the one and only SdFat instance, for readers.
//
// There is one SPI bus, one CS line and one card, so there is exactly one
// SdFat object, and it lives here because this is the module that brings the
// card up. replay.cpp reads recorded CSVs off that same card; giving it its
// own SdFat and SPIClass would mean two owners re-initialising the same bus
// underneath each other, which is a good way to wedge the card mid-demo.
//
// Returns nullptr when the card is not mounted. The call routes through the
// same begin()-with-cooldown retry that every write goes through, so a reader
// gets the benefit of recovery without duplicating the logic -- and cannot
// fire a back-to-back begin() either.
//
// The returned handle is only valid until the next call: a failed write marks
// the card unready and the following call may re-mount it, which invalidates
// any FsFile opened beforehand. Callers keeping a file open across ticks must
// be prepared for reads on it to start failing.
SdFat *loggerSdCard();
