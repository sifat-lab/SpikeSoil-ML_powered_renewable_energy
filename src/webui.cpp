#include "webui.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <math.h>

#include "replay.h"
#include "secrets.h"
#include "webui_page.h"

namespace {

constexpr uint16_t kHttpPort = 80;

// 300 samples at 1 Hz = the last 300 seconds.
constexpr uint16_t kHistSlots = 300;

// The replay ring is longer because its samples are 5 s apart, not 1 s: 600
// rows is 50 minutes of recorded time, which is enough to hold a whole soiling
// session on screen at once. That matters for the 31 July recording -- the
// climb from clean glass to 50 g/m2 is the thing being shown, and a window
// that scrolled it off the left would lose the argument.
constexpr uint16_t kReplayHistSlots = 600;

// Flush the history response about every kilobyte instead of building the whole
// ~10 kB body in RAM first.
constexpr uint16_t kChunkBytes = 1024;

// Display-only clamp. The raw model output is shipped alongside it untouched;
// a regression head landing at -0.02 or 1.03 is information, not something to
// quietly erase.
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

inline bool finiteVal(float v) { return !isnan(v) && !isinf(v); }

// JSON has no NaN or Infinity. Every non-finite value becomes null so the page
// can render a gap instead of a lie.
// dp is unsigned int, not uint8_t: String has both a (float, unsigned int) and
// an integer (value, base) constructor, and a uint8_t makes the call ambiguous.
String jnum(float v, unsigned int dp) {
  if (!finiteVal(v)) return F("null");
  return String(v, dp);
}

// Filenames come off a FAT directory, so they are not trusted to be clean JSON
// string content even though this logger only ever writes log_*.csv.
String jstr(const char *s) {
  String out;
  out.reserve(strlen(s) + 8);
  out += '"';
  for (const char *p = s; *p != '\0'; ++p) {
    const char c = *p;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

struct Snapshot {
  uint8_t state;
  float loss;
  float lossEma;
  float spikeRate;
  float lux;
  float iB_mA;
  float vB;
  float pB_mW;
  float iA_mA;  // panel A, for reference; instantaneous, not a 1 s mean
  unsigned long macs;
  uint32_t latencyUs;
  uint32_t stampMs;

  bool hasValid;
  float lastValidLoss;
  float lastValidEma;
  uint32_t lastValidMs;

  // Cost of the last inference that actually ran. Separate from the last-valid
  // loss latch above: that one answers "what was the panel doing", this one
  // answers "what does a forward pass cost", and the gate closing does not make
  // the second question stale. Zeros in these three cells read as dead silicon.
  bool hasRun;
  float lastRunRate;
  unsigned long lastRunMacs;
  uint32_t lastRunLatencyUs;
  uint32_t lastRunMs;

  LoopTiming timing;
};

struct HistEntry {
  uint32_t t;   // seconds since boot
  float loss;   // NAN while gated -> null in JSON -> gap in the chart
  float lux;
  float iB;
};

// Payback inputs. Held in RAM only: no NVS writes, and nothing on the web path
// ever touches the SD card.
struct Config {
  float tariff;      // BDT per kWh
  float cleanCost;   // BDT per cleaning
  float arrayWp;     // array rating the payback is being argued for
  float sunHours;    // peak sun hours per day
};

WebServer g_server(kHttpPort);

// No locking anywhere in this file, deliberately: the synchronous WebServer
// runs its handlers inside webuiHandleClient(), which main.cpp calls from
// loop() -- the same task that calls webuiPublish(). Producer and consumer are
// one thread and cannot interleave. If this ever moves back to an async server
// with its own task, the snapshot copy and the history ring both need guarding
// again.
Snapshot g_snap{};
bool g_snapReady = false;

HistEntry g_hist[kHistSlots];
uint32_t g_histTotal = 0;

// Replay's own snapshot, ring and latch. Nothing here is ever written by the
// live path and nothing in the live pair above is ever written by replay, so
// "did that number come from the panel or from the card" is answered by which
// variable it lives in, not by a flag that could be read at the wrong moment.
Snapshot g_rsnap{};
bool g_rsnapReady = false;

HistEntry g_rhist[kReplayHistSlots];
uint32_t g_rhistTotal = 0;

// arrayWp defaults to a whole rooftop installation, not the 100 Wp test panel
// on the bench. At 100 Wp one cleaning pays back in roughly 400 days even at
// 36% loss, which reads as broken economics when it is only a scale mismatch:
// the loss fraction is a property of the glass, the payback is a property of
// how much array is behind it.
Config g_cfg{8.0f, 500.0f, 3000.0f, 4.5f};
bool g_timeSynced = false;

float readFloatParam(const char *name, float fallback) {
  // WebServer parses a form-encoded body and the query string into the same
  // arg table, so the page and a bare curl behave identically.
  if (!g_server.hasArg(name)) return fallback;

  const String raw = g_server.arg(name);
  if (raw.length() == 0) return fallback;
  const float v = raw.toFloat();
  if (!finiteVal(v) || v < 0.0f) return fallback;
  return v;
}

String configJson() {
  String j;
  j.reserve(128);
  j += F("{\"tariff\":");
  j += String(g_cfg.tariff, 3);
  j += F(",\"cleanCost\":");
  j += String(g_cfg.cleanCost, 2);
  j += F(",\"arrayWp\":");
  j += String(g_cfg.arrayWp, 1);
  j += F(",\"sunHours\":");
  j += String(g_cfg.sunHours, 2);
  j += '}';
  return j;
}

const char *replayPlayName(ReplayPlay p) {
  switch (p) {
    case ReplayPlay::Playing:  return "playing";
    case ReplayPlay::Paused:   return "paused";
    case ReplayPlay::Finished: return "finished";
    default:                   return "stopped";
  }
}

// The block the banner is built from. Emitted inside /api/state on every poll
// so the page can never render a frame in which the loss is from a recording
// but the banner has not caught up yet -- source and value arrive together, in
// one document, or not at all.
String replayJson() {
  const ReplayStatus &st = replayStatus();
  const ReplayTiming &t = replayTiming();

  String j;
  j.reserve(576);
  j += F("{\"mode\":\"");
  j += (st.mode == ReplayMode::Replay) ? F("replay") : F("live");
  j += F("\",\"play\":\"");
  j += replayPlayName(st.play);
  j += F("\",\"speed\":");
  j += String(st.speed);
  j += F(",\"rowPeriodMs\":");
  j += String(st.rowPeriodMs);
  j += F(",\"file\":");
  j += jstr(st.file);
  j += F(",\"rowStamp\":");
  j += jstr(st.rowStamp);
  j += F(",\"rowEpoch\":");
  j += String(st.rowEpoch);
  j += F(",\"rowIndex\":");
  j += String(st.rowIndex);
  j += F(",\"recordedS\":");
  j += String(st.recordedS);
  j += F(",\"bytePos\":");
  j += String(st.bytePos);
  j += F(",\"byteTotal\":");
  j += String(st.byteTotal);
  j += F(",\"error\":");
  j += jstr(st.error);

  j += F(",\"timing\":{\"rows\":");
  j += String(t.rowsEmitted);
  j += F(",\"skipped\":");
  j += String(t.rowsSkipped);
  j += F(",\"lateRows\":");
  j += String(t.lateRows);
  j += F(",\"maxLateMs\":");
  j += String(t.maxLateMs);
  j += F(",\"meanLateMs\":");
  j += String(t.rowsEmitted ? (static_cast<float>(t.sumLateMs) / t.rowsEmitted) : 0.0f, 2);
  j += F(",\"resyncs\":");
  j += String(t.resyncs);
  j += F(",\"catchUpRows\":");
  j += String(t.catchUpRows);
  j += F(",\"minRowMs\":");
  j += String(t.rowsEmitted > 1 ? t.minRowMs : 0);
  j += F(",\"maxRowMs\":");
  j += String(t.rowsEmitted > 1 ? t.maxRowMs : 0);
  j += F("}}");
  return j;
}

void handleRoot() {
  g_server.send_P(200, "text/html; charset=utf-8", kIndexHtml);
}

void handleState() {
  // Which snapshot answers depends on who owns the model right now. The rest
  // of the document is identical in both modes, which is the point: the page
  // renders the same widgets either way and the banner says where the numbers
  // came from.
  const bool replaying = replayIsActive();
  const Snapshot &s = replaying ? g_rsnap : g_snap;
  const bool ready = replaying ? g_rsnapReady : g_snapReady;
  const uint32_t nowMs = millis();

  // Sized for the whole document including the replay block, so the once-a-
  // second poll does not walk a String through four reallocations on the way.
  String j;
  j.reserve(1792);  // +the last-run block
  j += F("{\"ready\":");
  j += ready ? F("true") : F("false");

  const InferState st = static_cast<InferState>(s.state);
  j += F(",\"state\":\"");
  j += ready ? inferStateName(st) : "booting";
  j += F("\",\"valid\":");
  j += (ready && st == InferState::Valid) ? F("true") : F("false");

  // Raw first, clamped second, and both are labelled. The page shows the
  // clamped one large; anyone reading the JSON still sees what the model said.
  j += F(",\"loss\":");
  j += jnum(s.loss, 4);
  j += F(",\"lossDisplay\":");
  j += finiteVal(s.loss) ? String(clamp01(s.loss), 4) : String(F("null"));
  j += F(",\"lossEma\":");
  j += jnum(s.lossEma, 4);

  // Last valid loss and its age. During a wipe the gate closes for ~45 s; the
  // page shows this instead of blanking, because a blank screen reads as a
  // fault to anyone standing at the panel.
  j += F(",\"lastValidLoss\":");
  j += (ready && s.hasValid) ? String(s.lastValidLoss, 4) : String(F("null"));
  j += F(",\"lastValidEma\":");
  j += (ready && s.hasValid) ? String(s.lastValidEma, 4) : String(F("null"));
  j += F(",\"secondsSinceValid\":");
  if (ready && s.hasValid) {
    j += String((nowMs - s.lastValidMs) / 1000);
  } else {
    j += F("null");
  }

  j += F(",\"lux\":");
  j += jnum(s.lux, 0);
  j += F(",\"iB_mA\":");
  j += jnum(s.iB_mA, 1);
  j += F(",\"vB\":");
  j += jnum(s.vB, 3);
  j += F(",\"pB_mW\":");
  j += jnum(s.pB_mW, 1);
  j += F(",\"iA_mA\":");
  j += jnum(s.iA_mA, 1);

  j += F(",\"spikeRate\":");
  j += jnum(s.spikeRate, 4);
  j += F(",\"macs\":");
  j += String(s.macs);
  j += F(",\"latencyUs\":");
  j += String(s.latencyUs);

  // The same three figures from the last forward pass that actually happened,
  // with their age. While the gate is closed the live trio above is 0/0/0 --
  // truthfully, since no pass ran -- and the page shows these instead.
  const bool haveRun = ready && s.hasRun;
  j += F(",\"lastRunRate\":");
  j += haveRun ? jnum(s.lastRunRate, 4) : String(F("null"));
  j += F(",\"lastRunMacs\":");
  j += haveRun ? String(s.lastRunMacs) : String(F("null"));
  j += F(",\"lastRunLatencyUs\":");
  j += haveRun ? String(s.lastRunLatencyUs) : String(F("null"));
  j += F(",\"secondsSinceRun\":");
  j += haveRun ? String((nowMs - s.lastRunMs) / 1000) : String(F("null"));

  j += F(",\"uptimeS\":");
  j += String(nowMs / 1000);
  j += F(",\"ageMs\":");
  j += String(ready ? (nowMs - s.stampMs) : 0);
  j += F(",\"timeSynced\":");
  j += g_timeSynced ? F("true") : F("false");
  j += F(",\"heap\":");
  j += String(ESP.getFreeHeap());
  j += F(",\"clients\":");
  j += String(WiFi.softAPgetStationNum());

  // Window fill after a mode switch. The 12-slot window is shared, so a switch
  // purges it (see replay.h) and the gate would report "sensor fault" for the
  // twelve steps that follow. That reads as broken hardware at a demo, so the
  // page is given the real reason instead.
  j += F(",\"windowFill\":");
  j += String(replayWindowFill());
  j += F(",\"windowNeeded\":");
  j += String(replayWindowNeeded());

  // Always the live loop's counters, in both modes. Replay does not perturb
  // them and must not appear to: they keep whatever they held when replay
  // started, and replay.timing below reports the replay clock separately.
  const LoopTiming &t = g_snap.timing;
  j += F(",\"timing\":{\"ticks\":");
  j += String(t.ticks);
  j += F(",\"steps\":");
  j += String(t.steps);
  j += F(",\"lateTicks\":");
  j += String(t.lateTicks);
  j += F(",\"maxLateMs\":");
  j += String(t.maxLateMs);
  j += F(",\"meanLateMs\":");
  j += String(t.ticks ? (static_cast<float>(t.sumLateMs) / t.ticks) : 0.0f, 2);
  j += F(",\"resyncs\":");
  j += String(t.resyncs);
  j += F(",\"minStepMs\":");
  j += String(t.steps > 1 ? t.minStepMs : 0);
  j += F(",\"maxStepMs\":");
  j += String(t.steps > 1 ? t.maxStepMs : 0);
  j += F("},\"config\":");
  j += configJson();
  j += F(",\"replay\":");
  j += replayJson();
  j += '}';

  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), j);
}

// One ring or the other, never a merge.
//
// The two use different time axes and that is deliberate. Live entries are
// stamped with seconds since boot; replay entries with seconds *into the
// recording*, so the chart's "-120s" reads as two minutes of recorded time
// rather than two minutes of wall clock -- at 20x those differ by a factor of
// twenty and only one of them is a fact about the panel.
void handleHistory() {
  const bool replaying = replayIsActive();
  const HistEntry *ring = replaying ? g_rhist : g_hist;
  const uint16_t slots = replaying ? kReplayHistSlots : kHistSlots;
  const uint32_t total = replaying ? g_rhistTotal : g_histTotal;
  const uint32_t nowT = replaying ? replayStatus().recordedS : (millis() / 1000);

  const uint16_t n = (total >= slots) ? slots : static_cast<uint16_t>(total);
  const uint32_t first = total - n;

  // Chunked, so the rows never exist as one large String.
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  g_server.send(200, F("application/json"), "");

  String buf;
  buf.reserve(kChunkBytes + 128);
  buf += F("{\"source\":\"");
  buf += replaying ? F("replay") : F("live");
  buf += F("\",\"stepS\":");
  buf += replaying ? F("5") : F("1");
  buf += F(",\"now\":");
  buf += String(nowT);
  buf += F(",\"n\":");
  buf += String(n);
  buf += F(",\"cols\":[\"t\",\"loss\",\"lux\",\"iB\"],\"rows\":[");

  for (uint16_t i = 0; i < n; ++i) {
    const HistEntry &e = ring[(first + i) % slots];
    if (i) buf += ',';
    buf += '[';
    buf += String(e.t);
    buf += ',';
    buf += jnum(e.loss, 4);
    buf += ',';
    buf += jnum(e.lux, 0);
    buf += ',';
    buf += jnum(e.iB, 1);
    buf += ']';
    if (buf.length() >= kChunkBytes) {
      g_server.sendContent(buf);
      buf.remove(0);  // length 0, capacity kept
    }
  }
  buf += F("]}");
  g_server.sendContent(buf);
  g_server.sendContent("");  // terminates the chunked body
}

void handleConfig() {
  g_cfg.tariff = readFloatParam("tariff", g_cfg.tariff);
  g_cfg.cleanCost = readFloatParam("cleanCost", g_cfg.cleanCost);
  // Not in the original spec, but "every input visible and editable" needs the
  // array rating and the sun-hours assumption too, and a phone reload should
  // not silently revert them to someone else's numbers.
  g_cfg.arrayWp = readFloatParam("arrayWp", g_cfg.arrayWp);
  g_cfg.sunHours = readFloatParam("sunHours", g_cfg.sunHours);

  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), configJson());
}

// The file picker's contents. Served from the cached directory listing, so a
// phone polling the dashboard never triggers an SD scan; ?rescan=1 is the only
// thing that walks the card again.
void handleFiles() {
  if (g_server.hasArg("rescan")) replayRescan();

  const uint8_t n = replayFileCount();
  String j;
  j.reserve(64 + n * 64);
  j += F("{\"count\":");
  j += String(n);
  j += F(",\"selected\":");
  j += jstr(replayStatus().file);
  j += F(",\"files\":[");
  for (uint8_t i = 0; i < n; ++i) {
    if (i) j += ',';
    j += F("{\"name\":");
    j += jstr(replayFileName(i));
    j += F(",\"size\":");
    j += String(replayFileSize(i));
    j += '}';
  }
  j += F("]}");

  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), j);
}

// The whole replay control surface, one endpoint, every field optional:
//   ?mode=live|replay & file=<name> & speed=1..60 & cmd=play|pause|restart
//
// Applied in that order on purpose. Selecting a file while the node is still
// LIVE only records the choice -- it must not reach into the shared window --
// and the mode switch that follows in the same request is what opens it. Doing
// it the other way round would open the previously selected file first and
// replay a second of the wrong recording before switching.
void handleReplayCtl() {
  if (g_server.hasArg("rescan")) replayRescan();

  if (g_server.hasArg("file")) {
    const String f = g_server.arg("file");
    if (f.length() > 0) replaySelectFile(f.c_str());
  }
  if (g_server.hasArg("speed")) {
    const long v = g_server.arg("speed").toInt();
    if (v > 0) replaySetSpeed(static_cast<uint8_t>(v > 255 ? 255 : v));
  }
  if (g_server.hasArg("mode")) {
    const String m = g_server.arg("mode");
    if (m == "replay") {
      replaySetMode(ReplayMode::Replay);
    } else if (m == "live") {
      replaySetMode(ReplayMode::Live);
    }
  }
  if (g_server.hasArg("cmd")) {
    const String c = g_server.arg("cmd");
    if (c == "play") {
      replayPlay();
    } else if (c == "pause") {
      replayPause();
    } else if (c == "restart") {
      replayRestart();
    }
  }

  // Anything that put the reader back at row zero -- entering replay, changing
  // file, restarting -- also drops the replay view, so the page cannot show a
  // number left over from the previous recording next to the new filename.
  if (replayStatus().rowIndex == 0) {
    g_rsnapReady = false;
    g_rhistTotal = 0;
  }

  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), replayJson());
}

// Browsers ask for this unprompted on every page load, and a phone that keeps
// the tab open asks again. 204 answers it in one segment with no body and,
// more to the point, keeps it out of the 404 path.
void handleFavicon() {
  g_server.send(204, F("text/plain"), "");
}

// Deliberately silent: nothing is written to Serial here at any level. The
// serial log is the demo's diagnostic channel, and a stray probe from a phone's
// captive-portal check is not a fault worth printing. The body is plain text
// and short so an unknown path costs one small segment and one loop tick.
void handleNotFound() {
  g_server.send(404, F("text/plain"), F("not found"));
}

// Snapshot fill plus the last-valid latch, shared by both publish paths so the
// two modes cannot drift apart in how they present a gated reading.
//
// The latch lives here, not in inference.cpp: the gate deliberately drops the
// last value (it clears the EMA prime on purpose), so carrying it forward is a
// presentation decision and belongs on the presentation side. prev is the
// caller's own previous snapshot, which is what keeps the live and replay
// latches independent.
Snapshot buildSnapshot(const SensorReading &r, const InferResult &res, const Snapshot &prev,
                       uint32_t nowMs, bool *validOut) {
  Snapshot s;
  s.state = static_cast<uint8_t>(res.state);
  s.loss = res.loss;
  s.lossEma = res.lossEma;
  s.spikeRate = res.spikeRate;
  s.lux = res.lux;
  s.iB_mA = res.iB_mA;
  s.vB = r.vB_V;
  s.pB_mW = r.pB_mW;
  s.iA_mA = r.iA_mA;
  s.macs = res.macs;
  s.latencyUs = res.latencyUs;
  s.stampMs = nowMs;
  s.timing = prev.timing;

  // Every gated path in inferenceRun() returns before snn_infer() and leaves
  // latencyUs at its initialised 0, so a non-zero latency is the exact test for
  // "the kernel ran this timestep" -- including the rare run that produced a
  // non-finite loss, whose cycle count is still a real measurement.
  const bool ran = (res.latencyUs != 0);
  if (ran) {
    s.hasRun = true;
    s.lastRunRate = res.spikeRate;
    s.lastRunMacs = res.macs;
    s.lastRunLatencyUs = res.latencyUs;
    s.lastRunMs = nowMs;
  } else {
    s.hasRun = prev.hasRun;
    s.lastRunRate = prev.lastRunRate;
    s.lastRunMacs = prev.lastRunMacs;
    s.lastRunLatencyUs = prev.lastRunLatencyUs;
    s.lastRunMs = prev.lastRunMs;
  }

  const bool valid = (res.state == InferState::Valid) && finiteVal(res.loss);
  if (valid) {
    s.hasValid = true;
    s.lastValidLoss = res.loss;
    s.lastValidEma = res.lossEma;
    s.lastValidMs = nowMs;
  } else {
    s.hasValid = prev.hasValid;
    s.lastValidLoss = prev.lastValidLoss;
    s.lastValidEma = prev.lastValidEma;
    s.lastValidMs = prev.lastValidMs;
  }

  *validOut = valid;
  return s;
}

}  // namespace

bool webuiBegin() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_AP);

  const bool up = WiFi.softAP(AP_SSID, AP_PASSWORD);
  if (!up) {
    Serial.println("[web] softAP failed; no dashboard this session");
    return false;
  }

  Serial.printf("[web] AP \"%s\" up, open http://%s/\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());

  g_server.on("/", HTTP_GET, handleRoot);
  g_server.on("/api/state", HTTP_GET, handleState);
  g_server.on("/api/history", HTTP_GET, handleHistory);
  g_server.on("/api/config", HTTP_POST, handleConfig);
  g_server.on("/api/files", HTTP_GET, handleFiles);
  // Both verbs: the page POSTs a form body, and a bare curl with a query
  // string is the quickest way to drive replay from a laptop on the AP.
  g_server.on("/api/replay", HTTP_POST, handleReplayCtl);
  g_server.on("/api/replay", HTTP_GET, handleReplayCtl);
  g_server.on("/favicon.ico", HTTP_GET, handleFavicon);
  g_server.onNotFound(handleNotFound);

  g_server.begin();
  return true;
}

// Called from loop() on every sub-sample tick (4 Hz). Synchronous: a request
// is parsed and answered inside this call, so it blocks the loop for as long
// as the response takes. That cost is deliberately left visible in
// LoopTiming::maxLateMs rather than hidden behind a second task.
void webuiHandleClient() { g_server.handleClient(); }

void webuiSetTimeSynced(bool synced) { g_timeSynced = synced; }

void webuiPublish(const SensorReading &r, const InferResult &res, const LoopTiming &t) {
  const uint32_t nowMs = millis();

  bool valid = false;
  Snapshot s = buildSnapshot(r, res, g_snap, nowMs, &valid);
  s.timing = t;

  g_snap = s;
  g_snapReady = true;

  HistEntry e;
  e.t = nowMs / 1000;
  e.loss = valid ? res.loss : NAN;
  e.lux = res.lux;
  e.iB = res.iB_mA;
  g_hist[g_histTotal % kHistSlots] = e;
  g_histTotal += 1;
}

void webuiPublishReplay(const SensorReading &r, const InferResult &res) {
  const ReplayStatus &st = replayStatus();

  // Row one of a pass: a restart or a file change put the reader back to the
  // top, so clear the view before the new pass writes into it. Cheap enough to
  // do unconditionally, and it makes the reset correct whether it was reached
  // through /api/replay or by the file simply being reopened.
  if (st.rowIndex <= 1) {
    g_rhistTotal = 0;
    g_rsnap = Snapshot{};
  }

  const uint32_t nowMs = millis();
  bool valid = false;
  g_rsnap = buildSnapshot(r, res, g_rsnap, nowMs, &valid);
  g_rsnapReady = true;

  // Stamped in recorded seconds rather than wall clock -- see handleHistory().
  HistEntry e;
  e.t = st.recordedS;
  e.loss = valid ? res.loss : NAN;
  e.lux = res.lux;
  e.iB = res.iB_mA;
  g_rhist[g_rhistTotal % kReplayHistSlots] = e;
  g_rhistTotal += 1;
}
