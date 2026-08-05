#pragma once

// Replay mode: the demo's weather-independent fallback.
//
// Reads a CSV recorded by logger.cpp back off the SD card and feeds it through
// the same window buffer and the same snn_infer as live mode, at 1x to 60x.
//
// The point is that it is the same code path, not a simulation. Every replayed
// row goes through inferencePush() and inferenceRun() exactly as a live sample
// does -- there is no second copy of the window, no second gate, and no second
// kernel call site. The only things replay owns are where the numbers come
// from and when they arrive. inference.*, logger.*, sensors.* and src/bench/
// are unmodified, save for one read-only accessor onto the shared SD handle
// (see loggerSdCard() in logger.h).
//
// Replay does not touch the SD log: while a recording is on screen the live
// sampling path in main.cpp does not run at all, so no CSV rows are written
// and no replayed sample reaches the live history ring.

#include <stdint.h>

#include "inference.h"
#include "sensors.h"

enum class ReplayMode : uint8_t {
  Live = 0,    // sensors drive the model; the SD log is being written
  Replay = 1   // a recorded CSV drives the model; the SD log is paused
};

enum class ReplayPlay : uint8_t {
  Stopped,   // no file open
  Playing,
  Paused,
  Finished   // ran off the end of the file; restart to go again
};

// Replay clock audit, the counterpart of LoopTiming. Live mode's cadence is
// held by main.cpp; this one measures whether the replay clock actually
// delivered rows at the requested speed, and it is reported next to the live
// counters rather than instead of them.
struct ReplayTiming {
  uint32_t rowsEmitted;   // data rows pushed into the model
  uint32_t rowsSkipped;   // malformed, short or over-long rows stepped over
  uint32_t lateRows;      // rows emitted more than kLateThresholdMs after their slot
  uint32_t maxLateMs;     // worst single row lateness
  uint64_t sumLateMs;     // for the mean
  uint32_t resyncs;       // long stalls that reset the clock instead of bursting
  uint32_t catchUpRows;   // rows emitted back-to-back to make up lost ground
  uint32_t minRowMs;      // observed wall-clock spacing extremes; both want
  uint32_t maxRowMs;      // 5000/speed ms
};

// Everything the dashboard needs to say, unmistakably, that what is on screen
// is a recording. The string pointers are owned by replay.cpp and stay valid
// until the next call into this module; nothing here is ever null.
struct ReplayStatus {
  ReplayMode mode;
  ReplayPlay play;
  uint8_t speed;          // 1..60
  uint32_t rowPeriodMs;   // wall-clock ms per row at the current speed
  const char *file;       // selected filename, "" when none
  const char *rowStamp;   // timestamp column of the row being replayed
  uint32_t rowEpoch;      // its epoch column, 0 if the recording had no clock
  uint32_t rowIndex;      // data rows consumed from this file since restart
  uint32_t recordedS;     // recorded seconds into the session, = rowIndex * 5
  uint32_t bytePos;       // progress through the file, exact and free
  uint32_t byteTotal;
  const char *error;      // "" when healthy
};

// Scans the card for CSVs and preselects the default recording. Safe to call
// with no card present; it just reports the error and finds no files.
void replayInit();

// True while a recording owns the model. main.cpp uses this to suspend live
// sampling, SD logging and the live history ring in one branch.
bool replayIsActive();

// Drives the replay clock. Call from loop() on every iteration, not on the
// 250 ms sub-sample tick: at 60x a row is due every 83 ms and the tick is too
// coarse to deliver that. Emits zero or more timesteps and returns quickly
// when nothing is due.
void replayService(uint32_t nowMs);

const ReplayStatus &replayStatus();
const ReplayTiming &replayTiming();

// ---------------------------------------------------------------------------
// Control surface. Called from the HTTP handlers in webui.cpp.
// ---------------------------------------------------------------------------

// Switching modes flushes the shared window (see below) in both directions.
void replaySetMode(ReplayMode m);

// Opens name from the card root and rewinds to the first data row. Returns
// false and sets status().error if it cannot be opened.
bool replaySelectFile(const char *name);

void replaySetSpeed(uint8_t speed);  // clamped to 1..60
void replayPlay();
void replayPause();
void replayRestart();  // back to the first data row, window and counters cleared

// Cached directory listing, so a phone polling the dashboard never triggers a
// scan. Rescan is explicit.
void replayRescan();
uint8_t replayFileCount();
const char *replayFileName(uint8_t i);  // "" if i is out of range
uint32_t replayFileSize(uint8_t i);

// Handed a finished timestep so replay.cpp needs no dependency on webui.* and
// builds unchanged with WEBUI_ENABLED=0. main.cpp installs it.
typedef void (*ReplayEmitFn)(const SensorReading &r, const InferResult &res);
void replaySetEmitHandler(ReplayEmitFn fn);

// ---------------------------------------------------------------------------
// Shared-window bookkeeping.
//
// The 12-slot window inside inference.cpp is a single global, and both modes
// push into it. A mode switch therefore has to purge it: otherwise the first
// eleven windows afterwards are a blend of live and recorded timesteps, and
// the model happily returns a number for a window that never existed.
//
// The purge is twelve all-NaN pushes through the ordinary inferencePush().
// That trips the existing NaN gate before the kernel runs -- so no cycles are
// spent on mixed data and no bogus loss is ever produced -- and it clears the
// EMA prime, which is exactly right across a switch.
//
// The counter below exists so the dashboard can say "refilling window" for the
// twelve steps that follow instead of "sensor fault", which is what the gate
// would otherwise report and which reads as broken hardware at a demo.
// ---------------------------------------------------------------------------

void replayFlushWindow();

// Call once after every inferencePush(), from whichever mode made it.
void replayNoteWindowPush();

uint8_t replayWindowFill();    // pushes since the flush, saturating at the window length
uint8_t replayWindowNeeded();  // the window length, = SNN_T
