#include "replay.h"

#include <Arduino.h>
#include <SdFat.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "logger.h"

// Included for SNN_T alone -- the window length has to agree with the model or
// the purge on a mode switch is the wrong size, and a hand-copied 12 here
// would drift silently the day the window changes. The weight arrays this
// header also carries are `static const` and unreferenced in this translation
// unit, so -fdata-sections/--gc-sections drops them; nothing is duplicated.
#if defined(SNN_MODEL_HEADER)
#include SNN_MODEL_HEADER
#else
#error "No SNN_MODEL_HEADER: replay cannot know the window length. Add -DSNN_MODEL_HEADER to the esp32s3 env."
#endif

namespace {

// ---------------------------------------------------------------------------
// Recording geometry
// ---------------------------------------------------------------------------

// logger.cpp writes one row every kStepsPerLog (5) timesteps at 1 Hz. That is
// the number replay's clock is scaled against: at 20x a 5 s row is due every
// 250 ms.
constexpr uint32_t kRecordedRowMs = 5000;

// One CSV row becomes one timestep, which is what "the same code path" means
// here -- inferencePush() is called once per row, exactly as live mode calls
// it once per 1 s mean. Worth stating plainly because the three cadences in
// this project are all different: ml_05 trained on 25 s means, live mode
// pushes 1 s means, and replay pushes the 5 s rows as recorded. Replay is
// therefore the closest of the three to the training distribution, not a
// looser approximation of live mode.

constexpr uint8_t kMinSpeed = 1;
constexpr uint8_t kMaxSpeed = 60;
constexpr uint8_t kDefaultSpeed = 20;

// Matches main.cpp's kLateThresholdMs so the two timing reports mean the same
// thing when they are read side by side.
constexpr uint32_t kLateThresholdMs = 50;

// A stalled loop must not be paid back as a burst: four rows is enough to ride
// out an HTTP response, and past that the clock resyncs and says so.
constexpr uint8_t kMaxCatchUpRows = 4;

// Bounds the work one service() call can do stepping over damaged lines, so a
// corrupt file cannot hold loop() indefinitely. Progress through the file is
// still made on every call.
constexpr uint8_t kMaxSkipPerRow = 32;

// ---------------------------------------------------------------------------
// CSV schema, as written by logger.cpp:
//   timestamp,epoch,vA,iA_mA,pA_mW,vB,iB_mA,pB_mW,lux,tA_C,tB_C
// ---------------------------------------------------------------------------
enum : uint8_t {
  kColTimestamp = 0,
  kColEpoch = 1,
  kColVA = 2,
  kColIA = 3,
  kColPA = 4,
  kColVB = 5,
  kColIB = 6,
  kColPB = 7,
  kColLux = 8,
  kColTA = 9,
  kColTB = 10,
  kColCount = 11
};

// Rows run about 100 bytes. 224 leaves room for a longer timestamp without
// making truncation common, and truncation is handled rather than assumed away.
constexpr uint16_t kLineLen = 224;
constexpr uint8_t kStampLen = 32;
constexpr uint8_t kNameLen = 40;
constexpr uint8_t kMaxFiles = 24;

// The session the demo is built around: a walk from clean glass to 50 g/m2, so
// the loss estimate climbs across the recording instead of sitting flat. The
// exact name is tried first, then any name carrying the same date, then
// whatever the card has.
constexpr char kPreferredFile[] = "log_20260731.csv";
constexpr char kPreferredToken[] = "0731";

struct FileEntry {
  char name[kNameLen];
  uint32_t size;
};

FileEntry g_files[kMaxFiles];
uint8_t g_fileCount = 0;

ReplayMode g_mode = ReplayMode::Live;
ReplayPlay g_play = ReplayPlay::Stopped;
uint8_t g_speed = kDefaultSpeed;

char g_selName[kNameLen] = "";
char g_rowStamp[kStampLen] = "";
const char *g_error = "";

FsFile g_file;
bool g_fileOpen = false;
uint32_t g_resumePos = 0;  // byte offset kept across a trip through LIVE
uint32_t g_bytePos = 0;
uint32_t g_byteTotal = 0;
uint32_t g_rowIndex = 0;
uint32_t g_rowEpoch = 0;

uint32_t g_nextRowMs = 0;
uint32_t g_lastRowMs = 0;

ReplayTiming g_timing{};
ReplayEmitFn g_emit = nullptr;

uint8_t g_windowFill = 0;

char g_line[kLineLen];

void setError(const char *e) { g_error = e; }
void clearError() { g_error = ""; }

uint32_t rowPeriodMs() {
  const uint32_t p = kRecordedRowMs / g_speed;
  return p ? p : 1;
}

bool endsWithCsv(const char *name) {
  const size_t n = strlen(name);
  if (n < 4) return false;
  return strcasecmp(name + n - 4, ".csv") == 0;
}

// ---------------------------------------------------------------------------
// File handling
// ---------------------------------------------------------------------------

void closeFile() {
  if (!g_fileOpen) return;
  g_resumePos = static_cast<uint32_t>(g_file.curPosition());
  g_file.close();
  g_fileOpen = false;
}

bool openFile() {
  if (g_fileOpen) return true;
  if (g_selName[0] == '\0') {
    setError("no recording selected");
    return false;
  }

  SdFat *sd = loggerSdCard();
  if (sd == nullptr) {
    setError("SD card not mounted");
    return false;
  }

  char path[kNameLen + 2];
  snprintf(path, sizeof(path), "/%s", g_selName);

  g_file = sd->open(path, O_RDONLY);
  if (!g_file) {
    setError("cannot open recording");
    return false;
  }
  g_fileOpen = true;
  g_byteTotal = static_cast<uint32_t>(g_file.fileSize());

  // Resume where the last visit to this file left off. A trip out to LIVE and
  // back is a mode switch, not a restart, and the operator would have to find
  // their place again otherwise.
  if (g_resumePos > 0 && g_resumePos < g_byteTotal) {
    g_file.seekSet(g_resumePos);
  } else {
    g_resumePos = 0;
  }
  g_bytePos = static_cast<uint32_t>(g_file.curPosition());
  clearError();
  return true;
}

// 1 a line was read, 0 end of file, -1 read error.
//
// A line longer than the buffer is drained to the next newline and reported as
// a read so the caller can count it as skipped -- the alternative is silently
// treating its tail as a fresh row.
int readLine(bool *truncated) {
  *truncated = false;
  const int n = g_file.fgets(g_line, sizeof(g_line));
  if (n <= 0) return n == 0 ? 0 : -1;

  if (g_line[n - 1] == '\n') {
    g_line[n - 1] = '\0';
    return 1;
  }

  // No newline: either the final line of a file that does not end in one, or a
  // line too long for the buffer. Draining settles it either way.
  if (n == static_cast<int>(sizeof(g_line)) - 1) {
    *truncated = true;
    char sink[64];
    while (true) {
      const int m = g_file.fgets(sink, sizeof(sink));
      if (m <= 0) break;
      if (sink[m - 1] == '\n') break;
    }
  }
  return 1;
}

// ---------------------------------------------------------------------------
// CSV parsing
// ---------------------------------------------------------------------------

// An empty field is NAN, which is exactly what logger.cpp's csvField() writes
// for a channel that failed to read. A field that is present but not a number
// is a damaged row, not a NaN -- the row gets skipped rather than quietly
// feeding the model a hole it did not have when it was recorded.
bool parseFloatField(const char *s, float *out) {
  if (*s == '\0') {
    *out = NAN;
    return true;
  }
  char *end = nullptr;
  const float v = strtof(s, &end);
  if (end == s) return false;
  while (*end == ' ' || *end == '\t' || *end == '\r') ++end;
  if (*end != '\0') return false;
  *out = v;
  return true;
}

// Splits line in place. Returns false for the header row, a short row, or any
// row with an unparseable numeric field.
bool parseRow(char *line, SensorReading *r, char *stamp, size_t stampLen, uint32_t *epoch) {
  char *field[kColCount];
  uint8_t n = 0;

  char *p = line;
  field[n++] = p;
  while (*p != '\0' && n < kColCount) {
    if (*p == ',') {
      *p = '\0';
      field[n++] = p + 1;
    }
    ++p;
  }
  if (n < kColCount) return false;

  // The header, and nothing else, opens with the literal column name.
  if (strncmp(field[kColTimestamp], "timestamp", 9) == 0) return false;
  if (field[kColTimestamp][0] == '\0') return false;

  // Any trailing columns beyond the schema stay attached to the last field;
  // cut them off so tB_C still parses if a column is ever appended.
  char *tail = strchr(field[kColTB], ',');
  if (tail != nullptr) *tail = '\0';

  float v[kColCount];
  for (uint8_t c = kColVA; c < kColCount; ++c) {
    if (!parseFloatField(field[c], &v[c])) return false;
  }

  char *end = nullptr;
  const unsigned long ep = strtoul(field[kColEpoch], &end, 10);
  *epoch = (end == field[kColEpoch]) ? 0u : static_cast<uint32_t>(ep);

  r->vA_V = v[kColVA];
  r->iA_mA = v[kColIA];
  r->pA_mW = v[kColPA];
  r->vB_V = v[kColVB];
  r->iB_mA = v[kColIB];
  r->pB_mW = v[kColPB];
  r->lux = v[kColLux];
  r->tA_C = v[kColTA];
  r->tB_C = v[kColTB];

  strlcpy(stamp, field[kColTimestamp], stampLen);
  return true;
}

// ---------------------------------------------------------------------------
// The replay clock
// ---------------------------------------------------------------------------

void noteRow(uint32_t nowMs, uint32_t scheduledMs) {
  const uint32_t lateMs = nowMs - scheduledMs;  // >= 0 by the caller's guard
  g_timing.sumLateMs += lateMs;
  if (lateMs > g_timing.maxLateMs) g_timing.maxLateMs = lateMs;
  if (lateMs > kLateThresholdMs) g_timing.lateRows += 1;

  if (g_timing.rowsEmitted > 0) {
    const uint32_t periodMs = nowMs - g_lastRowMs;
    if (periodMs < g_timing.minRowMs || g_timing.minRowMs == 0) {
      g_timing.minRowMs = periodMs;
    }
    if (periodMs > g_timing.maxRowMs) g_timing.maxRowMs = periodMs;
  }
  g_lastRowMs = nowMs;
  g_timing.rowsEmitted += 1;
}

// Reads forward to the next usable row and puts it through the model. Returns
// false at end of file, on a read error, or when the skip budget for this call
// runs out.
bool emitOneRow() {
  for (uint8_t attempt = 0; attempt < kMaxSkipPerRow; ++attempt) {
    bool truncated = false;
    const int n = readLine(&truncated);
    if (n == 0) {
      g_play = ReplayPlay::Finished;
      return false;
    }
    if (n < 0) {
      setError("SD read failed");
      g_play = ReplayPlay::Finished;
      closeFile();
      return false;
    }

    SensorReading r;
    char stamp[kStampLen];
    uint32_t epoch = 0;
    if (truncated || !parseRow(g_line, &r, stamp, sizeof(stamp), &epoch)) {
      g_timing.rowsSkipped += 1;
      continue;
    }

    // The whole point of the mode: the identical two calls live mode makes.
    inferencePush(r);
    replayNoteWindowPush();
    const InferResult res = inferenceRun();

    strlcpy(g_rowStamp, stamp, sizeof(g_rowStamp));
    g_rowEpoch = epoch;
    g_rowIndex += 1;
    g_bytePos = static_cast<uint32_t>(g_file.curPosition());

    if (g_emit != nullptr) g_emit(r, res);
    return true;
  }
  return false;  // skip budget spent; the next call picks up where this left off
}

// File position and counters only. The window purge is deliberately NOT done
// here: picking a file from the dropdown while the node is still LIVE must not
// reach into the model and blank twelve seconds of live inference. Callers that
// own the model flush it themselves.
void rewindToStart() {
  g_resumePos = 0;
  if (g_fileOpen) {
    g_file.seekSet(0);
    g_bytePos = 0;
  }
  g_rowIndex = 0;
  g_rowEpoch = 0;
  g_rowStamp[0] = '\0';
  g_timing = ReplayTiming{};
  g_lastRowMs = 0;
}

uint8_t pickDefaultFile() {
  for (uint8_t i = 0; i < g_fileCount; ++i) {
    if (strcasecmp(g_files[i].name, kPreferredFile) == 0) return i;
  }
  for (uint8_t i = 0; i < g_fileCount; ++i) {
    if (strstr(g_files[i].name, kPreferredToken) != nullptr) return i;
  }
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared-window bookkeeping
// ---------------------------------------------------------------------------

void replayFlushWindow() {
  SensorReading blank;
  blank.vA_V = NAN;
  blank.iA_mA = NAN;
  blank.pA_mW = NAN;
  blank.vB_V = NAN;
  blank.iB_mA = NAN;
  blank.pB_mW = NAN;
  blank.lux = NAN;
  blank.tA_C = NAN;
  blank.tB_C = NAN;

  // Every slot NaN: inferenceRun()'s finite check trips on the first one, so
  // the kernel is never run against a window that mixes the two sources, and
  // the EMA prime is dropped along with it.
  for (uint8_t i = 0; i < SNN_T; ++i) inferencePush(blank);
  g_windowFill = 0;
}

void replayNoteWindowPush() {
  if (g_windowFill < SNN_T) g_windowFill += 1;
}

uint8_t replayWindowFill() { return g_windowFill; }
uint8_t replayWindowNeeded() { return SNN_T; }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void replayInit() {
  g_mode = ReplayMode::Live;
  g_play = ReplayPlay::Stopped;
  g_speed = kDefaultSpeed;
  g_windowFill = 0;
  g_timing = ReplayTiming{};

  replayRescan();
  if (g_fileCount == 0) {
    Serial.printf("[replay] no CSV recordings on the card (%s)\n",
                  g_error[0] ? g_error : "card readable, directory empty");
    return;
  }

  const uint8_t def = pickDefaultFile();
  strlcpy(g_selName, g_files[def].name, sizeof(g_selName));
  Serial.printf("[replay] %u recording%s found; default \"%s\" (%lu bytes)\n",
                g_fileCount, g_fileCount == 1 ? "" : "s", g_selName,
                static_cast<unsigned long>(g_files[def].size));
}

void replayRescan() {
  g_fileCount = 0;

  SdFat *sd = loggerSdCard();
  if (sd == nullptr) {
    setError("SD card not mounted");
    return;
  }

  FsFile root = sd->open("/", O_RDONLY);
  if (!root) {
    setError("cannot open card root");
    return;
  }

  FsFile f;
  while (g_fileCount < kMaxFiles && f.openNext(&root, O_RDONLY)) {
    if (!f.isDir()) {
      char name[kNameLen];
      f.getName(name, sizeof(name));
      if (endsWithCsv(name)) {
        strlcpy(g_files[g_fileCount].name, name, sizeof(g_files[0].name));
        g_files[g_fileCount].size = static_cast<uint32_t>(f.fileSize());
        g_fileCount += 1;
      }
    }
    f.close();
  }
  root.close();
  clearError();
}

uint8_t replayFileCount() { return g_fileCount; }

const char *replayFileName(uint8_t i) {
  return (i < g_fileCount) ? g_files[i].name : "";
}

uint32_t replayFileSize(uint8_t i) { return (i < g_fileCount) ? g_files[i].size : 0; }

void replaySetEmitHandler(ReplayEmitFn fn) { g_emit = fn; }

bool replayIsActive() { return g_mode == ReplayMode::Replay; }

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

void replaySetMode(ReplayMode m) {
  if (m == g_mode) return;
  g_mode = m;

  // Purge in both directions. Going in, the window still holds live seconds;
  // coming out, it still holds recorded rows. Either way the next eleven
  // windows would be a blend of the two if this were skipped.
  replayFlushWindow();

  if (m == ReplayMode::Replay) {
    if (openFile()) {
      g_play = ReplayPlay::Paused;  // the operator presses play
      g_nextRowMs = millis() + rowPeriodMs();
    } else {
      g_play = ReplayPlay::Stopped;
    }
    Serial.printf("[replay] mode REPLAY, file \"%s\", %ux%s%s\n", g_selName, g_speed,
                  g_error[0] ? ", error: " : "", g_error);
  } else {
    // Release the handle rather than hold it across live logging: a failed
    // write re-mounts the card, and an FsFile opened before that is stale.
    closeFile();
    g_play = ReplayPlay::Stopped;
    Serial.println("[replay] mode LIVE, sampling and SD logging resumed");
  }
}

bool replaySelectFile(const char *name) {
  if (name == nullptr || name[0] == '\0') return false;

  // Only names the scan actually found, so a crafted path cannot be handed
  // straight to open().
  uint8_t idx = kMaxFiles;
  for (uint8_t i = 0; i < g_fileCount; ++i) {
    if (strcmp(g_files[i].name, name) == 0) {
      idx = i;
      break;
    }
  }
  if (idx == kMaxFiles) {
    setError("no such recording on the card");
    return false;
  }
  if (strcmp(g_selName, name) == 0 && g_fileOpen) return true;

  closeFile();
  strlcpy(g_selName, g_files[idx].name, sizeof(g_selName));
  g_resumePos = 0;
  g_byteTotal = g_files[idx].size;
  rewindToStart();
  g_play = ReplayPlay::Stopped;

  if (g_mode != ReplayMode::Replay) return true;  // opened on the way in

  if (!openFile()) return false;
  replayFlushWindow();  // the window still holds rows from the previous file
  g_play = ReplayPlay::Paused;
  g_nextRowMs = millis() + rowPeriodMs();
  return true;
}

void replaySetSpeed(uint8_t speed) {
  if (speed < kMinSpeed) speed = kMinSpeed;
  if (speed > kMaxSpeed) speed = kMaxSpeed;
  if (speed == g_speed) return;
  g_speed = speed;

  // Re-anchor rather than let the old slot fire immediately at the new rate:
  // a jump from 1x to 60x would otherwise dump the backlog in one burst.
  g_nextRowMs = millis() + rowPeriodMs();
}

void replayPlay() {
  if (g_mode != ReplayMode::Replay) return;
  if (!openFile()) return;
  if (g_play == ReplayPlay::Finished) return;  // restart is the way back
  g_play = ReplayPlay::Playing;
  g_nextRowMs = millis() + rowPeriodMs();
}

void replayPause() {
  if (g_play == ReplayPlay::Playing) g_play = ReplayPlay::Paused;
}

void replayRestart() {
  if (g_mode != ReplayMode::Replay) {
    g_resumePos = 0;
    return;
  }
  if (!openFile()) return;
  rewindToStart();
  replayFlushWindow();  // replay owns the model here, so the purge is ours to do
  g_play = ReplayPlay::Playing;
  g_nextRowMs = millis() + rowPeriodMs();
  Serial.printf("[replay] restart \"%s\" at %ux\n", g_selName, g_speed);
}

// ---------------------------------------------------------------------------
// Service
// ---------------------------------------------------------------------------

void replayService(uint32_t nowMs) {
  if (g_mode != ReplayMode::Replay || g_play != ReplayPlay::Playing) return;

  const uint32_t periodMs = rowPeriodMs();
  uint8_t emitted = 0;

  while (emitted < kMaxCatchUpRows && static_cast<int32_t>(nowMs - g_nextRowMs) >= 0) {
    const uint32_t scheduledMs = g_nextRowMs;
    if (!emitOneRow()) break;
    noteRow(nowMs, scheduledMs);
    g_nextRowMs += periodMs;  // fixed cadence, no accumulated drift
    ++emitted;
  }
  if (emitted > 1) g_timing.catchUpRows += emitted - 1u;

  // Same policy as main.cpp's tick scheduler: past a few slots behind, resync
  // instead of replaying a burst nobody can read, and count it.
  if (static_cast<int32_t>(nowMs - g_nextRowMs) > static_cast<int32_t>(kMaxCatchUpRows * periodMs)) {
    g_nextRowMs = nowMs + periodMs;
    g_timing.resyncs += 1;
  }

  if (g_play == ReplayPlay::Finished) {
    Serial.printf("[replay] end of \"%s\": %lu rows replayed, %lu skipped\n", g_selName,
                  static_cast<unsigned long>(g_timing.rowsEmitted),
                  static_cast<unsigned long>(g_timing.rowsSkipped));
  }
}

const ReplayStatus &replayStatus() {
  static ReplayStatus s;
  s.mode = g_mode;
  s.play = g_play;
  s.speed = g_speed;
  s.rowPeriodMs = rowPeriodMs();
  s.file = g_selName;
  s.rowStamp = g_rowStamp;
  s.rowEpoch = g_rowEpoch;
  s.rowIndex = g_rowIndex;
  s.recordedS = g_rowIndex * (kRecordedRowMs / 1000u);
  s.bytePos = g_fileOpen ? g_bytePos : g_resumePos;
  s.byteTotal = g_byteTotal;
  s.error = g_error;
  return s;
}

const ReplayTiming &replayTiming() { return g_timing; }
