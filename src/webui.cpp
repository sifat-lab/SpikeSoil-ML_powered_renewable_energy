#include "webui.h"

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <math.h>

#include "secrets.h"
#include "webui_page.h"

namespace {

constexpr uint16_t kHttpPort = 80;

// 300 samples at 1 Hz = the last 300 seconds.
constexpr uint16_t kHistSlots = 300;

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

void handleRoot() {
  g_server.send_P(200, "text/html; charset=utf-8", kIndexHtml);
}

void handleState() {
  const Snapshot &s = g_snap;
  const bool ready = g_snapReady;
  const uint32_t nowMs = millis();

  String j;
  j.reserve(768);
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

  const LoopTiming &t = s.timing;
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
  j += '}';

  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.send(200, F("application/json"), j);
}

void handleHistory() {
  const uint32_t total = g_histTotal;
  const uint16_t n = (total >= kHistSlots) ? kHistSlots : static_cast<uint16_t>(total);
  const uint32_t first = total - n;

  // Chunked, so 300 rows never exist as one ~10 kB String.
  g_server.sendHeader(F("Cache-Control"), F("no-store"));
  g_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  g_server.send(200, F("application/json"), "");

  String buf;
  buf.reserve(kChunkBytes + 128);
  buf += F("{\"now\":");
  buf += String(millis() / 1000);
  buf += F(",\"n\":");
  buf += String(n);
  buf += F(",\"cols\":[\"t\",\"loss\",\"lux\",\"iB\"],\"rows\":[");

  for (uint16_t i = 0; i < n; ++i) {
    const HistEntry &e = g_hist[(first + i) % kHistSlots];
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
  s.timing = t;

  // The latch lives here, not in inference.cpp: the gate deliberately drops the
  // last value (it clears the EMA prime on purpose), so carrying it forward is
  // a presentation decision and belongs on the presentation side.
  const bool valid = (res.state == InferState::Valid) && finiteVal(res.loss);
  if (valid) {
    s.hasValid = true;
    s.lastValidLoss = res.loss;
    s.lastValidEma = res.lossEma;
    s.lastValidMs = nowMs;
  } else {
    s.hasValid = g_snap.hasValid;
    s.lastValidLoss = g_snap.lastValidLoss;
    s.lastValidEma = g_snap.lastValidEma;
    s.lastValidMs = g_snap.lastValidMs;
  }

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
