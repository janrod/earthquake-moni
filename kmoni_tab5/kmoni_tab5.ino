// Kyoshin Monitor (強震モニタ, http://www.kmoni.bosai.go.jp/) on the M5Stack Tab5.
//
// The Tab5 has no browser, so this rebuilds the page natively from the same data
// the page's own JavaScript loads once a second: the GIF layers and the EEW JSON.
// Layers stack as on the page: base map, estimated intensity, stations, P/S circles,
// then the colour scale. Swiping down from the top opens brightness, alarm volume, auto-return,
// the Wi-Fi selector, the location and night mode. Dragging in from the right edge opens JMA's list of recent earthquakes.
//
// Two tasks keep touch responsive: netTask (core 0) does everything that can block,
// fetching and building the map; loop() (core 1) owns the screen, touch and speaker.
// They share only `shared`, under `sharedLock`, and swap finished map layers. quakeTask, also
// on core 0 at a lower priority, handles the earthquake pages so they can't delay the live map.
//
// Serial (USB), for verification only:
//   S              dump the panel's framebuffer as raw RGB565 (see tools/screenshot.py)
//   R<14 digits>   replay from a past time, e.g. R20240101161030
//   L              back to live
//   I              print status counters
//   D              drop Wi-Fi, to exercise recovery
//   P<xxxx><yyyy>  inject a touch press or move, U to release (see tools/menu_test.py)
//   W              list the scanned networks
//   K              list the current keyboard page's keys
//   H              make the secrets.h network the last-used one, to test startup fallback
//   Q              list the earthquake rows
//   B              battery readings, raw and shown
//   N<hhmm>        dim and switch off as if it were that time; N---- returns to the clock
//   X              restart the way the no-data watchdog does

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <M5Unified.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <mbedtls/platform.h>
#include <time.h>

#include "battery_gauge.h"
#include "gif_layer.h"
#include "jma_root_ca.h"
#include "known_networks.h"
#include "location_editor.h"
#include "night_dimmer.h"
#include "night_mode.h"
#include "quake_browser.h"
#include "quake_data.h"
#include "quake_history.h"
#include "quake_snapshot.h"
#include "quake_map.h"
#include "resample.h"
#include "secrets.h"
#include "settings_menu.h"
#include "wifi_selector.h"

namespace {

// Layout. The map's 352x400 scales to the full 720 width; the rest of the
// portrait screen holds the clock above and the EEW box below, as on the page.
constexpr int kScreenW = 720, kScreenH = 1280;
constexpr int kSrcW = 352, kSrcH = 400;
constexpr int kMapW = kScreenW, kMapH = 818, kMapY = 118;
// The page draws the 55x301 scale at (305,101) in a 47x301 box on the map.
constexpr int kScaleSrcW = 55, kScaleSrcH = 301;
constexpr int kScaleW = 96, kScaleH = 616, kScaleX = 624, kScaleY = 207;
// The scale hangs 5 px below the map, as on the page, so the layer is a little taller.
constexpr int kLayerH = kScaleY + kScaleH;
constexpr int kBoxX = 16, kBoxY = 950, kBoxW = 688, kBoxH = 270;
constexpr int kFooterY = 1232;

const char *const kHost = "http://www.kmoni.bosai.go.jp";
constexpr size_t kMaxDownload = 256 * 1024;
constexpr uint32_t kAlarmRetryMs = 60 * 1000;
constexpr uint32_t kAlertFreshMs = 60 * 1000;
constexpr time_t kClockPlausibleFrom = 1577836800;  // 2020: earlier means it was never set
constexpr float kTokyoLat = 35.6581f, kTokyoLon = 139.7414f;  // dims by this with no location set
constexpr uint32_t kConnectTimeoutTotalMs = 20 * 1000;
constexpr int kSavedNetworkAttempts = 3;

// New connections to the server stall for ~5 s about a third of the time, so reuse
// one connection and give up quickly when connecting.
constexpr int kConnectTimeoutMs = 1500;
constexpr int kReadTimeoutMs = 2500;
constexpr int kFetchAttempts = 3;

constexpr uint32_t kClockResyncMs = 60 * 1000;  // same as the page
constexpr uint32_t kStaleAfterMs = 5 * 1000;
constexpr uint32_t kFirstJoinAfterMs = 10 * 1000;  // how long the last network gets at startup
constexpr uint32_t kWifiKickAfterMs = 30 * 1000;
constexpr uint32_t kRebootAfterMs = 5 * 60 * 1000;

constexpr uint32_t kInk = 0x222222, kGrey = 0x666666, kFaint = 0x999999, kPaper = 0xFFFFFF;
constexpr uint32_t kAlertRed = 0xFF3333, kOkGreen = 0x1E9E4A;
// Colours must stay uint32_t: LovyanGFX reads a plain int as RGB565.
// This grey is neutral after rounding to RGB565; 0xF2F2F2 turns pink.
constexpr uint32_t kPanelBg = 0xF0F4F0, kPanelLine = 0x999999, kPanelHandle = 0xAAAAAA;
constexpr uint32_t kPanelDisabled = 0xC8CCC8;

struct Eew {
  bool active = false;
  bool warning = false;
  bool forecast = false;
  String reportTime, reportLabel, region, magnitude, depth, intensity;
};

struct Wav {
  const char *path;
  uint8_t *data = nullptr;
  size_t len = 0;
};

// What netTask tells the UI.
struct Shared {
  time_t frameTime = 0;
  time_t clockTime = 0;  // server time at clockAtMs; 0 until first sync
  uint32_t clockAtMs = 0;
  Eew eew;
  uint32_t lastFrameMs = 0;
  uint32_t lastHypoMs = 0;
  bool haveFrame = false;
  bool replaying = false;
  uint32_t frameSeq = 0;
  uint16_t *layerFront = nullptr;  // only read or swapped under the lock
  enum Alarm { kNoAlarm, kAlarmForecast, kAlarmWarning } alarm = kNoAlarm;
  // UI requests
  bool replayRequested = false;
  time_t replayTarget = 0;  // 0 returns to live
};
Shared shared;
SemaphoreHandle_t sharedLock;

// The selector's traffic with netTask, also under sharedLock. Kept out of Shared,
// which render() copies onto its stack.
struct WifiExchange {
  bool scanRequested = false;
  bool scanDone = false;
  int scanCount = 0;
  WifiSelector::Network scanResults[WifiSelector::kMaxNetworks];
  bool connectRequested = false;
  char connectSsid[33] = {};
  char connectPass[WifiSelector::kMaxPassword + 1] = {};
  bool connectDone = false;
  bool connectOk = false;
  char currentSsid[33] = {};
};
WifiExchange wifiX;
volatile bool selectorOpen;  // reconnecting is held off, as it would block scanning

// The earthquake pages' traffic with quakeTask, also under sharedLock. quakeTask fills the
// buffers named here, and only while the matching Done flag is clear. loop() takes a finished
// one by swapping it with its own, so each side works without holding the lock.
constexpr int kQuakeRows = 256;     // the live list
constexpr int kOlderRows = kQuakeSnapshotCount + 3000;  // built in, then fetched from the database
static_assert(kOlderRows + kQuakeRows <= QuakeBrowser::kMaxRows, "the list must be able to show every row");
constexpr uint32_t kTopUpRetryMs = 60 * 1000;
constexpr int kOlderBatch = QuakeHistory::kDbRowLimit;
constexpr int kQuakeMapW = kScreenW, kQuakeMapH = 740;
struct QuakeExchange {
  bool listRequested = false;
  bool listDone = false, listOk = false;
  int listCount = 0;
  QuakeSummary *list = nullptr;

  bool olderRequested = false;
  QuakeDbRequest olderPlan = {};
  bool olderDone = false;
  int olderCount = -1;  // -1 failed
  QuakeSummary *olderBatch = nullptr;

  bool detailRequested = false;
  char detailJson[QuakeBrowser::kJsonNameSize] = {};
  bool detailDone = false, detailOk = false;
  char detailFor[QuakeBrowser::kJsonNameSize] = {};
  QuakeDetail *detail = nullptr;
  uint16_t *map = nullptr;
};
QuakeExchange quakeX;

struct Locked {
  Locked() { xSemaphoreTake(sharedLock, portMAX_DELAY); }
  ~Locked() { xSemaphoreGive(sharedLock); }
};

// --- netTask only ---
// All large buffers live in PSRAM.
uint8_t *download, *gifWork;
uint32_t *baseLayer, *composed, *upscaled, *scaleLayer;
uint16_t *layerBack;
ResampleAxis mapAxisX[kMapW], mapAxisY[kMapH];

WiFiClient tcp;
HTTPClient http;

time_t syncedTime = 0;  // server time at syncedAtMs; 0 until first sync
uint32_t syncedAtMs = 0;
uint32_t lastSyncAttemptMs = 0;
time_t replayFrom = 0;  // nonzero while replaying
uint32_t replayStartMs = 0;
time_t shownTime = 0;
uint32_t wifiDownSinceMs = 0;
String savedSsid, savedPass;  // the network to be on
KnownNetworks known;           // everything that has ever connected
bool scanRunning = false;
bool scanRetried = false;
bool firstJoinTried = false;
volatile uint32_t disconnectEvents = 0;
volatile uint8_t lastDisconnectReason = 0;
Eew eew;
String lastWarningId, lastForecastId;
Wav wavWarning{"/kyoshin_monitor/static/audio/eq.wav"};
Wav wavForecast{"/kyoshin_monitor/static/audio/Warning1.wav"};
uint32_t lastAlarmLoadMs = 0;

// --- read by the serial status command ---
volatile uint32_t framesShown, framesFellBack, framesFailed;
volatile bool dropWifiRequested;

// --- loop() only ---
M5Canvas canvas(&M5.Display);
M5Canvas panel(&M5.Display);
M5Canvas modal(&M5.Display);
SettingsMenu menu;
WifiSelector selector;
Preferences prefs;
uint32_t bootMs = 0;
uint32_t watchdogGraceFromMs = 0;
bool wifiFromNvs = false;
int panelShownPos = 0;
uint32_t touchesSeen = 0, alarmsPlayed = 0, uiWorstMs = 0;
bool injectActive = false, injectPressed = false;
int injectX = 0, injectY = 0;

// --- earthquake pages: loop() only ---
QuakeBrowser browser;
BatteryGauge battery;
EdgeSwipeDetector edgeSwipe;
PageSlide pageSlide;
QuakeSummary *quakeRows;  // the live list, followed logically by olderRows
int quakeLiveCount = 0;
QuakeSummary *olderRows;
int olderCount = 0;
int olderStart = 0;  // older rows before this one are also in the live list
uint32_t lastTopUpMs = 0;
QuakeHistory history;
QuakeDbRequest olderPlanSent = {};
LocationEditor locationEditor;
bool homeSet = false;
float homeLat = 0, homeLon = 0;
NightDimmer dimmer;
NightSchedule nightMode;
NightModeEditor nightEditor;
bool backlightOff = false;
SunTimes sunToday = {};
int dimMinute = -1;     // the minute of the day it's dimming for; -1 until the clock is known
int forcedMinute = -1;  // set over serial for testing
uint32_t lastAlarmMs = 0;
QuakeDetail *quakeShown;
uint16_t *quakeMapShown;

// --- quakeTask only ---
uint8_t *quakeDownload;
QuakeRegions quakeRegions;
constexpr size_t kQuakeDownloadCap = 1536 * 1024;  // the largest report seen is 981 KB
constexpr int kQuakeStationCap = 4000, kQuakeAreaCap = 256;
volatile uint32_t quakeFetchFailures;

// Task logs mustn't land inside a binary screen dump.
volatile bool serialDumping = false;
#define taskLog(...) do { if (!serialDumping) Serial.printf(__VA_ARGS__); } while (0)

template <typename T>
T *psramAlloc(size_t count) {
  void *p = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    taskLog("PSRAM alloc of %u failed, rebooting\n", (unsigned)(count * sizeof(T)));
    delay(2000);
    ESP.restart();
  }
  return static_cast<T *>(p);
}

// GET into `download`. Returns the HTTP status (body length in *outLen), or <= 0 on a
// transport failure after retries. Bodies are always drained so the connection survives.
int httpGet(const String &path, size_t *outLen) {
  int status = 0;
  for (int attempt = 0; attempt < kFetchAttempts; attempt++) {
    if (WiFi.status() != WL_CONNECTED) return 0;
    if (!http.begin(tcp, String(kHost) + path)) continue;
    http.setReuse(true);
    http.setConnectTimeout(kConnectTimeoutMs);
    http.setTimeout(kReadTimeoutMs);
    status = http.GET();
    if (status <= 0) {
      http.end();
      tcp.stop();
      continue;
    }
    const int expected = http.getSize();
    if (expected < 0 || (size_t)expected > kMaxDownload) {
      taskLog("bad length %d for %s\n", expected, path.c_str());
      http.end();
      tcp.stop();
      return 0;
    }
    WiFiClient *stream = http.getStreamPtr();
    size_t got = 0;
    const uint32_t started = millis();
    while (got < (size_t)expected && millis() - started < (uint32_t)kReadTimeoutMs) {
      const int n = stream->read(download + got, expected - got);
      if (n > 0) {
        got += n;
      } else {
        if (!stream->connected() && stream->available() == 0) break;
        delay(1);
      }
    }
    if (got == (size_t)expected) {
      http.end();
      *outLen = got;
      return status;
    }
    status = 0;  // short read: the connection is unusable
    http.end();
    tcp.stop();
  }
  return status;
}

bool fetchLayer(const String &path, uint32_t *dst, int w, int h) {
  size_t len = 0;
  if (httpGet(path, &len) != 200) return false;
  return gifDrawOver(download, len, dst, w, h, gifWork);
}

String stamp(time_t t) {
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char buf[16];
  strftime(buf, sizeof buf, "%Y%m%d%H%M%S", &tmv);
  return buf;
}

time_t parseStamp(const char *s) {
  struct tm tmv = {};
  if (sscanf(s, "%4d%2d%2d%2d%2d%2d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday, &tmv.tm_hour,
             &tmv.tm_min, &tmv.tm_sec) != 6)
    return 0;
  tmv.tm_year -= 1900;
  tmv.tm_mon -= 1;
  return mktime(&tmv);
}

// The server's clock drives frame selection, as on the page. It reports JST as wall
// time; we treat it as UTC throughout and never convert, so it displays as JST.
bool syncClock() {
  lastSyncAttemptMs = millis();
  size_t len = 0;
  if (httpGet("/webservice/server/pros/latest.json", &len) != 200) return false;
  JsonDocument doc;
  if (deserializeJson(doc, download, len)) return false;
  const char *latest = doc["latest_time"];  // "2026/11/03 09:45:10"
  if (!latest) return false;
  struct tm tmv = {};
  if (sscanf(latest, "%d/%d/%d %d:%d:%d", &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday, &tmv.tm_hour,
             &tmv.tm_min, &tmv.tm_sec) != 6)
    return false;
  tmv.tm_year -= 1900;
  tmv.tm_mon -= 1;
  syncedTime = mktime(&tmv);
  syncedAtMs = millis();
  // Survives a restart, so night mode needn't light the screen to find out the time again.
  const timeval tv = {syncedTime, 0};
  settimeofday(&tv, nullptr);
  Locked lock;
  shared.clockTime = syncedTime;
  shared.clockAtMs = syncedAtMs;
  return true;
}

time_t wantedTime() {
  if (replayFrom) return replayFrom + (millis() - replayStartMs) / 1000;
  return syncedTime + (millis() - syncedAtMs) / 1000;
}

// Same semantics as the page: flags are JSON booleans during an event and "" when idle.
bool jsonTrue(JsonVariantConst v) { return v.is<bool>() && v.as<bool>(); }

String jsonText(JsonVariantConst v) { return v.is<const char *>() ? String(v.as<const char *>()) : String(); }

// Same rule as the page: each level sounds once per report id, and a forecast that
// becomes a warning sounds again.
void noteAlarm(const String &id, bool warning, bool forecast) {
  if (id.isEmpty()) return;
  Shared::Alarm alarm = Shared::kNoAlarm;
  if (warning && id != lastWarningId) {
    lastWarningId = id;
    alarm = Shared::kAlarmWarning;
  } else if (forecast && id != lastForecastId) {
    lastForecastId = id;
    alarm = Shared::kAlarmForecast;
  }
  if (alarm != Shared::kNoAlarm) {
    Locked lock;
    shared.alarm = alarm;
  }
}

// Returns false only if the state could not be determined; `eew` is then left as it was.
bool fetchEew(const String &ts) {
  size_t len = 0;
  if (httpGet("/webservice/hypo/eew/" + ts + ".json", &len) != 200) return false;
  JsonDocument doc;
  if (deserializeJson(doc, download, len)) return false;
  if (jsonText(doc["result"]["status"]) != "success") return false;

  Eew e;
  const String num = jsonText(doc["report_num"]);
  if (num.length() > 0) {
    e.active = true;
    const String flag = jsonText(doc["alertflg"]);
    e.warning = flag == "警報";
    e.forecast = flag == "予報";
    e.reportTime = jsonText(doc["report_time"]);
    e.region = jsonText(doc["region_name"]);
    e.magnitude = jsonText(doc["magunitude"]);
    e.depth = jsonText(doc["depth"]);
    e.intensity = jsonText(doc["calcintensity"]);
    noteAlarm(jsonText(doc["report_id"]), e.warning, e.forecast);
    if (jsonTrue(doc["is_cancel"])) e.reportLabel = "キャンセル報";
    else if (jsonTrue(doc["is_training"])) e.reportLabel = "訓練報";
    else if (jsonTrue(doc["is_final"])) e.reportLabel = "最終報";
    else e.reportLabel = "第" + num + "報";
  }
  eew = e;
  return true;
}

// Fetches and composes the frame for `t`. False if there's nothing new to show.
bool buildFrame(time_t t) {
  const String ts = stamp(t);
  const String day = ts.substring(0, 8);

  memcpy(composed, baseLayer, kSrcW * kSrcH * sizeof(uint32_t));

  if (fetchEew(ts)) {
    Locked lock;
    shared.lastHypoMs = millis();
  }

  // A missing optional layer (the server redirects to nodata.gif) just isn't drawn.
  if (eew.active) {
    fetchLayer("/data/map_img/EstShindoImg/eew/" + day + "/" + ts + ".eew.gif", composed, kSrcW, kSrcH);
  }
  bool stations = fetchLayer("/data/map_img/RealTimeImg/jma_s/" + day + "/" + ts + ".jma_s.gif",
                             composed, kSrcW, kSrcH);
  if (eew.active) {
    fetchLayer("/data/map_img/PSWaveImg/eew/" + day + "/" + ts + ".eew.gif", composed, kSrcW, kSrcH);
  }
  // Station images age off the server; EEW layers don't, so replay runs without them.
  return stations || replayFrom != 0;
}

// Publishes `composed` as the frame for `t`, with the map scaled and the colour scale on top.
void publishFrame(time_t t) {
  resampleRgba(composed, kSrcW, mapAxisX, kMapW, mapAxisY, kMapH, upscaled);
  blitOverSwap565(layerBack, kScreenW, kLayerH, 0, 0, upscaled, kMapW, kMapH);
  memset(layerBack + (size_t)kMapH * kScreenW, 0xFF, (size_t)(kLayerH - kMapH) * kScreenW * 2);
  blitOverSwap565(layerBack, kScreenW, kLayerH, kScaleX, kScaleY, scaleLayer, kScaleW, kScaleH);

  Locked lock;
  uint16_t *old = shared.layerFront;
  shared.layerFront = layerBack;
  layerBack = old;
  shared.frameTime = t;
  shared.eew = eew;
  shared.lastFrameMs = millis();
  shared.haveFrame = true;
  shared.frameSeq++;
}

void text(const lgfx::IFont *font, textdatum_t datum, uint32_t color, uint32_t bg, const String &s, int x, int y) {
  canvas.setFont(font);
  canvas.setTextDatum(datum);
  canvas.setTextColor(color, bg);
  canvas.drawString(s, x, y);
}

// The Japanese Gothic fonts lack some kanji used in place names, such as the 騨 of 飛騨.
// Those names switch to this larger character set, at its one size.
const lgfx::IFont *placeNameFont(const char *name, const lgfx::IFont *preferred) {
  lgfx::FontMetrics metrics;
  for (const uint8_t *p = reinterpret_cast<const uint8_t *>(name); *p;) {
    uint16_t code = *p++;
    if (code >= 0xE0) {
      code = (code & 0x0F) << 12;
      if (p[0]) code |= (*p++ & 0x3F) << 6;
      if (p[0]) code |= *p++ & 0x3F;
    } else if (code >= 0xC0) {
      code = (code & 0x1F) << 6;
      if (p[0]) code |= *p++ & 0x3F;
    }
    if (!preferred->updateFontMetric(&metrics, code)) return &fonts::efontJA_24;
  }
  return preferred;
}

void drawHeader(time_t shownTime, bool stale) {
  canvas.fillRect(0, 0, kScreenW, kMapY, kPaper);
  text(&fonts::lgfxJapanGothic_40, top_left, kInk, kPaper, "強震モニタ", 24, 38);

  const uint32_t clock = stale ? kAlertRed : kInk;
  if (shownTime) {
    struct tm tmv;
    gmtime_r(&shownTime, &tmv);
    char date[32], hms[16];
    strftime(date, sizeof date, "%Y/%m/%d JST", &tmv);
    strftime(hms, sizeof hms, "%H:%M:%S", &tmv);
    text(&fonts::lgfxJapanGothic_24, top_right, clock, kPaper, date, 696, 14);
    text(&fonts::FreeSansBold24pt7b, top_right, clock, kPaper, hms, 696, 46);
  }
}

void drawEewBox(const Eew &eew, bool haveFrame, bool stale) {
  uint32_t bg = kPaper, border = 0x000090, ink = kGrey;  // the page's three box styles
  if (eew.active && eew.warning) {
    bg = 0xFFE5E5;
    border = kAlertRed;
    ink = kAlertRed;
  } else if (eew.active && eew.forecast) {
    bg = 0xFFFFF0;
    border = 0xFED22F;
  }
  canvas.fillRect(0, kBoxY - 6, kScreenW, kFooterY - kBoxY + 6, kPaper);
  canvas.fillSmoothRoundRect(kBoxX, kBoxY, kBoxW, kBoxH, 12, border);
  canvas.fillSmoothRoundRect(kBoxX + 3, kBoxY + 3, kBoxW - 6, kBoxH - 6, 10, bg);

  const int left = kBoxX + 24, right = kBoxX + kBoxW - 24;
  text(&fonts::lgfxJapanGothic_24, top_left, ink, bg, "緊急地震速報", left, kBoxY + 18);

  if (!eew.active) {
    // Never claim "no EEW" unless that came from a recent answer.
    const bool known = !stale && haveFrame;
    text(&fonts::lgfxJapanGothic_40, middle_center, known ? kFaint : kAlertRed, bg,
         known ? "発表なし" : "受信できません", kBoxX + kBoxW / 2, kBoxY + kBoxH / 2 + 10);
    return;
  }

  text(&fonts::lgfxJapanGothic_24, top_right, ink, bg, eew.reportTime, right, kBoxY + 18);

  const lgfx::IFont *regionFont = placeNameFont(eew.region.c_str(), &fonts::lgfxJapanGothic_40);
  canvas.setFont(regionFont);
  if (canvas.textWidth(eew.region) > right - left - 150) regionFont = placeNameFont(eew.region.c_str(), &fonts::lgfxJapanGothic_28);
  text(regionFont, top_left, ink, bg, eew.region, left, kBoxY + 62);
  text(&fonts::lgfxJapanGothic_28, top_right, ink, bg, eew.reportLabel, right, kBoxY + 68);

  // M, depth, max predicted intensity
  const int y0 = kBoxY + 130, y1 = kBoxY + 170;
  const int colW = (right - left) / 3;
  text(&fonts::lgfxJapanGothic_24, top_left, ink, bg, "M", left, y0);
  text(&fonts::lgfxJapanGothic_40, top_left, ink, bg, eew.magnitude, left, y1);
  text(&fonts::lgfxJapanGothic_24, top_left, ink, bg, "深さ", left + colW, y0);
  text(&fonts::lgfxJapanGothic_40, top_left, ink, bg, eew.depth, left + colW, y1);
  text(&fonts::lgfxJapanGothic_24, top_left, ink, bg, "最大予測震度", left + 2 * colW, y0);
  text(&fonts::lgfxJapanGothic_40, top_left, ink, bg, eew.intensity, left + 2 * colW, y1);
  if (eew.warning) {
    text(&fonts::lgfxJapanGothic_40, bottom_right, ink, bg, "警報", right, kBoxY + kBoxH - 12);
  }
}

// A small battery with its level, or a plug if there's no battery, at the right of the footer.
// Returns its left edge.
int drawBatteryIndicator(int y) {
  constexpr int kRight = 696, kW = 36, kH = 18;
  if (!battery.known()) return kRight;
  if (!battery.present()) {
    canvas.setFont(&fonts::lgfxJapanGothic_20);
    const int tw = canvas.textWidth("USB");
    text(&fonts::lgfxJapanGothic_20, top_right, kGrey, kPaper, "USB", kRight, y);
    const int x = kRight - tw - 8 - 30, cy = y + 10;
    canvas.fillRect(x, cy - 1, 9, 2, kGrey);                  // cord
    canvas.fillRoundRect(x + 9, cy - 7, 13, 14, 3, kGrey);    // body
    canvas.fillRect(x + 22, cy - 5, 7, 3, kGrey);             // pins
    canvas.fillRect(x + 22, cy + 2, 7, 3, kGrey);
    return x;
  }
  const bool known = battery.hasLevel();
  const int level = battery.level();
  canvas.setFont(&fonts::lgfxJapanGothic_20);
  const String label = known ? String(level) + "%" : String("--");
  const int tw = canvas.textWidth(label);
  text(&fonts::lgfxJapanGothic_20, top_right, kGrey, kPaper, label, kRight, y);
  const int x = kRight - tw - 8 - kW - 3, cy = y + 10;
  uint32_t fill = kOkGreen;
  if (known && !battery.charging() && !battery.external()) fill = level <= 15 ? kAlertRed : level <= 30 ? 0xE0A000u : kOkGreen;
  canvas.drawRoundRect(x, cy - kH / 2, kW, kH, 4, kGrey);
  canvas.fillRect(x + kW, cy - 3, 3, 6, kGrey);
  if (known && level > 0) {
    const int w = (kW - 6) * level / 100;
    canvas.fillRect(x + 3, cy - kH / 2 + 3, w < 2 ? 2 : w, kH - 6, fill);
  }
  if (battery.charging()) {  // a bolt, outlined so it shows over the fill
    const int m = x + kW / 2;
    canvas.fillTriangle(m + 3, cy - 9, m - 6, cy + 2, m + 1, cy + 2, kPaper);
    canvas.fillTriangle(m - 3, cy + 9, m + 6, cy - 2, m - 1, cy - 2, kPaper);
    canvas.fillTriangle(m + 2, cy - 7, m - 4, cy + 1, m + 1, cy + 1, kInk);
    canvas.fillTriangle(m - 2, cy + 7, m + 4, cy - 1, m - 1, cy - 1, kInk);
  }
  return x;
}

void drawFooter(bool haveFrame, bool replaying, bool stale, uint32_t ageMs) {
  canvas.fillRect(0, kFooterY - 4, kScreenW, kScreenH - kFooterY + 4, kPaper);
  text(&fonts::lgfxJapanGothic_20, top_left, kFaint, kPaper, "© 防災科学技術研究所 (NIED) 強震モニタ", 24, kFooterY + 6);
  String status;
  uint32_t color = kOkGreen;
  if (replaying) {
    status = "再生中";
    color = kAlertRed;
  } else if (WiFi.status() != WL_CONNECTED) {
    status = "Wi-Fi 接続中...";
    color = kAlertRed;
  } else if (stale) {
    status = haveFrame ? "更新停止 " + String(ageMs / 1000) + "秒" : "接続中...";
    color = kAlertRed;
  } else {
    status = "受信中";
  }
  const int left = drawBatteryIndicator(kFooterY + 6);
  text(&fonts::lgfxJapanGothic_20, top_right, color, kPaper, status, left - 14, kFooterY + 6);
}

void applySound(int percent) { M5.Speaker.setVolume((uint8_t)(percent * 255 / 100)); }

// The present on the server's clock, as JST seconds; 0 until the first sync. A replay doesn't move it.
time_t clockNow() {
  Locked lock;
  return shared.clockTime ? shared.clockTime + (millis() - shared.clockAtMs) / 1000 : 0;
}

// Follows the day, a touch and any alert. The clock here is never the one a replay moves.
void updateBrightness(bool pressed) {
  const uint32_t now = millis();
  const time_t clock = clockNow();
  bool alert;
  {
    Locked lock;
    // A lost connection can't hold the screen at full forever.
    alert = shared.eew.active && now - shared.lastFrameMs < kAlertFreshMs;
  }
  // The alarm sounds a frame ahead of the map that shows why.
  if (lastAlarmMs && now - lastAlarmMs < kAlertFreshMs) alert = true;
  dimMinute = forcedMinute;
  int day = 0;
  if (clock) {
    struct tm tmv;
    gmtime_r(&clock, &tmv);
    if (dimMinute < 0) dimMinute = tmv.tm_hour * 60 + tmv.tm_min;
    day = tmv.tm_yday;
  }
  // Worked out again only when the day or the location changes.
  static int sunDay = -1;
  static float sunLat = 0, sunLon = 0;
  const float lat = homeSet ? homeLat : kTokyoLat, lon = homeSet ? homeLon : kTokyoLon;
  if (day != sunDay || lat != sunLat || lon != sunLon) {
    sunDay = day;
    sunLat = lat;
    sunLon = lon;
    sunToday = sunTimesJst(lat, lon, day);
  }
  // Level 0 turns off the backlight alone, and frames are still drawn, so it comes back showing
  // the present. Measured on this unit: M5.Display.sleep() and the panel's display-off command
  // both stop the touch panel reporting, and a touch could no longer wake it.
  const int level = dimmer.update(now, pressed, alert, dimMinute, sunToday, menu.brightness(), nightMode.screenOff(dimMinute));
  backlightOff = level == 0;
  if (level != (int)M5.Display.getBrightness()) M5.Display.setBrightness((uint8_t)level);
}

void drawPanelButton(SettingsMenu::Button b, bool plus) {
  const auto r = SettingsMenu::buttonRect(b);
  const bool enabled = menu.canApply(b);
  const uint32_t fill = menu.armed() == b ? 0xC8D4F0 : kPaper;
  const uint32_t mark = enabled ? kInk : kPanelDisabled;
  panel.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 14, enabled ? kGrey : kPanelDisabled);
  panel.fillSmoothRoundRect(r.x + 2, r.y + 2, r.w - 4, r.h - 4, 12, fill);
  const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  panel.fillRect(cx - 26, cy - 3, 52, 6, mark);
  if (plus) panel.fillRect(cx - 3, cy - 26, 6, 52, mark);
}

// Shortens to fit, on a UTF-8 boundary, as SSIDs can be Japanese.
template <typename Gfx>
String fitText(Gfx &g, String s, int maxWidth) {
  if (g.textWidth(s) <= maxWidth) return s;
  while (s.length() > 0 && g.textWidth(s + "...") > maxWidth) {
    int end = s.length() - 1;
    while (end > 0 && ((uint8_t)s[end] & 0xC0) == 0x80) end--;
    s.remove(end);
  }
  return s + "...";
}

String currentSsid() {
  Locked lock;
  return wifiX.currentSsid;
}

void drawPanelWifiRow() {
  const auto r = SettingsMenu::buttonRect(SettingsMenu::kWifi);
  const uint32_t fill = menu.armed() == SettingsMenu::kWifi ? 0xC8D4F0 : kPaper;
  panel.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 14, kGrey);
  panel.fillSmoothRoundRect(r.x + 2, r.y + 2, r.w - 4, r.h - 4, 12, fill);
  const int cy = r.y + r.h / 2;
  const bool connected = WiFi.status() == WL_CONNECTED;
  panel.setFont(&fonts::lgfxJapanGothic_24);
  panel.setTextColor(kGrey, fill);
  panel.setTextDatum(middle_left);
  panel.drawString("Wi-Fi", r.x + 24, cy);
  panel.setTextColor(connected ? kOkGreen : kAlertRed, fill);
  panel.setTextDatum(middle_center);
  panel.drawString(connected ? fitText(panel, currentSsid(), 400) : String("Not connected"), kScreenW / 2, cy);
  panel.setTextColor(kGrey, fill);
  panel.setTextDatum(middle_right);
  panel.drawString(">", r.x + r.w - 24, cy);
}

// A row that opens another screen, with its current value in the middle.
void drawPanelLinkRow(SettingsMenu::Button b, const char *label, const String &value, uint32_t ink) {
  const auto r = SettingsMenu::buttonRect(b);
  const uint32_t fill = menu.armed() == b ? 0xC8D4F0 : kPaper;
  panel.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 14, kGrey);
  panel.fillSmoothRoundRect(r.x + 2, r.y + 2, r.w - 4, r.h - 4, 12, fill);
  const int cy = r.y + r.h / 2;
  panel.setFont(&fonts::lgfxJapanGothic_24);
  panel.setTextColor(kGrey, fill);
  panel.setTextDatum(middle_left);
  panel.drawString(label, r.x + 24, cy);
  panel.setTextColor(ink, fill);
  panel.setTextDatum(middle_center);
  panel.drawString(value, kScreenW / 2 + 30, cy);
  panel.setTextColor(kGrey, fill);
  panel.setTextDatum(middle_right);
  panel.drawString(">", r.x + r.w - 24, cy);
}

String clockText(int minute) {
  char t[6];
  snprintf(t, sizeof t, "%02d:%02d", minute / 60, minute % 60);
  return t;
}

String nightModeText() {
  if (nightMode.mode == NightSchedule::kContinuous) return "Continuous";
  if (nightMode.mode == NightSchedule::kScheduled) return clockText(nightMode.start) + " - " + clockText(nightMode.end);
  return "Off";
}

void drawPanelRow(int row, const char *label, const String &value, SettingsMenu::Button minus, SettingsMenu::Button plus) {
  const uint32_t bg = kPanelBg;
  const int y = SettingsMenu::kRowY[row];
  drawPanelButton(minus, false);
  drawPanelButton(plus, true);
  panel.setTextDatum(top_center);
  panel.setTextColor(kGrey, bg);
  panel.setFont(&fonts::lgfxJapanGothic_24);
  panel.drawString(label, kScreenW / 2, y + 8);
  panel.setTextColor(kInk, bg);
  panel.setFont(&fonts::FreeSansBold24pt7b);
  panel.drawString(value, kScreenW / 2, y + 48);
}

void drawPanel() {
  panel.fillScreen(kPanelBg);
  drawPanelRow(0, "Brightness", String(menu.brightness()) + "%", SettingsMenu::kBrightMinus, SettingsMenu::kBrightPlus);
  drawPanelRow(1, "Sound", menu.sound() == 0 ? String("Off") : String(menu.sound()) + "%", SettingsMenu::kSoundMinus, SettingsMenu::kSoundPlus);
  const int autoSeconds = menu.autoReturnSeconds();
  drawPanelRow(2, "Auto-return", autoSeconds == 0 ? String("Off") : String(autoSeconds) + " s", SettingsMenu::kAutoMinus, SettingsMenu::kAutoPlus);
  drawPanelWifiRow();
  drawPanelLinkRow(SettingsMenu::kLocation, "Location", homeSet ? String(homeLat, 4) + ", " + String(homeLon, 4) : String("Not set"), homeSet ? kInk : kFaint);
  drawPanelLinkRow(SettingsMenu::kNight, "Night mode", nightModeText(), nightMode.mode == NightSchedule::kOff ? kFaint : kInk);
  panel.fillSmoothRoundRect(kScreenW / 2 - 40, SettingsMenu::kPanelH - 16, 80, 6, 3, kPanelHandle);
  panel.fillRect(0, SettingsMenu::kPanelH - 3, kScreenW, 3, kPanelLine);
}

// Copies rows of the main screen from the canvas, leaving the rest of the panel untouched.
void pushCanvasRows(int from, int to) {
  if (from >= to) return;
  M5.Display.setClipRect(0, from, kScreenW, to - from);
  canvas.pushSprite(0, 0);
  M5.Display.clearClipRect();
}

// Moves the panel on screen, restoring whatever it stops covering.
void showPanelAt(int pos) {
  M5.Display.startWrite();
  if (pos < panelShownPos) pushCanvasRows(pos, panelShownPos);
  if (pos > 0) panel.pushSprite(0, pos - SettingsMenu::kPanelH);
  M5.Display.endWrite();
  panelShownPos = pos;
}

// Rebuilds the canvas from the latest frame and state, without showing it.
Shared composeCanvas() {
  Shared s;
  {
    Locked lock;
    s.frameTime = shared.frameTime;
    s.eew = shared.eew;
    s.lastFrameMs = shared.lastFrameMs;
    s.lastHypoMs = shared.lastHypoMs;
    s.haveFrame = shared.haveFrame;
    s.replaying = shared.replaying;
    memcpy(static_cast<uint16_t *>(canvas.getBuffer()) + (size_t)kMapY * kScreenW, shared.layerFront,
           (size_t)kScreenW * kLayerH * 2);
  }
  // After the lock, so `now` can't be earlier than the timestamps just read.
  const uint32_t now = millis();
  const uint32_t age = now - s.lastFrameMs;
  const uint32_t hypoAge = now - s.lastHypoMs;
  const bool stale = !s.haveFrame || age > kStaleAfterMs || hypoAge > kStaleAfterMs;
  drawHeader(s.frameTime, stale);
  drawEewBox(s.eew, s.haveFrame, stale);
  drawFooter(s.haveFrame, s.replaying, stale, age > hypoAge ? age : hypoAge);
  return s;
}

// Rebuilds the canvas and shows it around the panel.
void render() {
  const Shared s = composeCanvas();
  const uint32_t now = millis();

  M5.Display.startWrite();
  pushCanvasRows(panelShownPos, kScreenH);
  if (panelShownPos > 0) panel.pushSprite(0, panelShownPos - SettingsMenu::kPanelH);
  M5.Display.endWrite();

  // Long outage: recover from anything a fresh start would fix.
  const uint32_t lastGood = s.haveFrame ? s.lastFrameMs : bootMs;
  // Not for a Wi-Fi outage while the screen is off: rejoining doesn't need a restart, and each one
  // lights the screen for a moment during startup.
  if (now - lastGood > kRebootAfterMs && now - watchdogGraceFromMs > kRebootAfterMs &&
      (!backlightOff || WiFi.status() == WL_CONNECTED)) {
    taskLog("no data for 5 min, rebooting\n");
    ESP.restart();
  }
}

// --- Wi-Fi selector screens ---
constexpr uint32_t kHeaderBg = 0xF0F4F0, kArmedBg = 0xC8D4F0;
constexpr uint32_t kRowLine = 0xE0E4E0, kKeyEdge = 0xB8BCB8;
// For password characters. The Japanese fonts draw a backslash as a yen sign, and this
// one also tells 0 from O and 1 from l.
const lgfx::IFont *const kPasswordFont = &fonts::FreeMonoBold18pt7b;

void drawModalHeader(const String &title, bool backArmed) {
  modal.fillRect(0, 0, kScreenW, WifiSelector::kHeaderH, kHeaderBg);
  modal.fillRect(0, WifiSelector::kHeaderH - 2, kScreenW, 2, kPanelLine);
  if (backArmed) modal.fillRect(WifiSelector::kBack.x, WifiSelector::kBack.y, WifiSelector::kBack.w, WifiSelector::kBack.h - 2, kArmedBg);
  modal.setFont(&fonts::lgfxJapanGothic_28);
  modal.setTextDatum(middle_left);
  modal.setTextColor(kInk);
  modal.drawString("< Back", 20, WifiSelector::kHeaderH / 2);
  modal.setTextDatum(middle_center);
  modal.drawString(fitText(modal, title, 340), kScreenW / 2, WifiSelector::kHeaderH / 2);
}

void drawSignalBars(int x, int y, int rssi) {
  const int level = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
  for (int i = 0; i < 4; i++) {
    const int h = 10 + i * 8;
    modal.fillRect(x + i * 12, y + 34 - h, 8, h, i < level ? kInk : 0xD0D4D0);
  }
}

void drawLock(int x, int y, uint32_t bg) {
  modal.fillRoundRect(x + 4, y, 16, 20, 7, kGrey);
  modal.fillRoundRect(x + 8, y + 4, 8, 14, 3, bg);
  modal.fillRect(x, y + 12, 24, 18, kGrey);
}

void drawListView() {
  modal.fillScreen(kPaper);
  const int count = selector.networkCount();
  modal.setClipRect(0, WifiSelector::kListTop, kScreenW, kScreenH - WifiSelector::kListTop);
  for (int i = 0; i < count; i++) {
    const auto r = WifiSelector::rowRect(i, selector.scrollY());
    if (r.y + r.h <= WifiSelector::kListTop || r.y >= kScreenH) continue;
    const auto &n = selector.network(i);
    const bool unsupported = n.security == WifiSelector::kUnsupported;
    const uint32_t bg = selector.litRow() == i ? kArmedBg : kPaper;
    if (bg != kPaper) modal.fillRect(r.x, r.y, r.w, r.h, bg);
    modal.fillRect(24, r.y + r.h - 2, kScreenW - 48, 2, kRowLine);
    const bool isCurrent = selector.isConnected(i);
    modal.setFont(&fonts::lgfxJapanGothic_28);
    modal.setTextDatum(top_left);
    modal.setTextColor(unsupported ? kFaint : kInk);
    modal.drawString(fitText(modal, n.ssid, 520), 24, r.y + (isCurrent || unsupported || n.known ? 16 : 34));
    modal.setFont(&fonts::lgfxJapanGothic_20);
    if (isCurrent) {
      modal.setTextColor(kOkGreen);
      modal.drawString("Connected", 24, r.y + 58);
    } else if (unsupported) {
      modal.setTextColor(kFaint);
      modal.drawString("Security type not supported", 24, r.y + 58);
    } else if (n.known) {
      modal.setTextColor(kGrey);
      modal.drawString("Saved", 24, r.y + 58);
    }
    if (n.security == WifiSelector::kPsk) drawLock(590, r.y + 34, bg);
    else if (n.security == WifiSelector::kOpen) {
      modal.setTextColor(kGrey);
      modal.setTextDatum(middle_right);
      modal.drawString("Open", 620, r.y + r.h / 2);
    }
    drawSignalBars(636, r.y + 32, n.rssi);
  }
  modal.clearClipRect();
  if (count == 0) {
    modal.setFont(&fonts::lgfxJapanGothic_28);
    modal.setTextDatum(middle_center);
    modal.setTextColor(kGrey);
    modal.drawString(selector.scanning() ? "Scanning..." : "No networks found", kScreenW / 2, 500);
  }
  drawModalHeader(selector.failed() ? "Couldn't connect" : "Wi-Fi", selector.backArmed());
  if (selector.rescanArmed()) modal.fillRect(WifiSelector::kRescan.x, 0, WifiSelector::kRescan.w, WifiSelector::kHeaderH - 2, kArmedBg);
  modal.setFont(&fonts::lgfxJapanGothic_28);
  modal.setTextDatum(middle_right);
  modal.setTextColor(selector.scanning() ? kFaint : kInk);
  modal.drawString("Rescan", 700, WifiSelector::kHeaderH / 2);
}

String keyLabel(const WifiSelector::Key &k) {
  switch (k.kind) {
    case WifiSelector::kShift: return "Shift";
    case WifiSelector::kBackspace: return "Del";
    case WifiSelector::kPage: return selector.page() == WifiSelector::kSymbols ? "abc" : "#+=";
    case WifiSelector::kSpace: return "Space";
    case WifiSelector::kConnect: return "Connect";
    default: return String(k.ch);
  }
}

void drawKeyboard() {
  WifiSelector::Key keys[WifiSelector::kMaxKeys];
  const int n = selector.keys(keys);
  for (int i = 0; i < n; i++) {
    const auto &k = keys[i];
    const bool isChar = k.kind == WifiSelector::kChar;
    const bool connect = k.kind == WifiSelector::kConnect;
    const bool enabled = !connect || selector.canConnect();
    uint32_t fill = isChar || k.kind == WifiSelector::kSpace ? kPaper : 0xE0E4E0;
    uint32_t ink = enabled ? kInk : 0xB0B4B0;
    if (connect && enabled) {
      fill = 0x1E6FD9;
      ink = kPaper;
    }
    if (k.kind == WifiSelector::kShift && selector.shift() != WifiSelector::kShiftOff) {
      fill = selector.shift() == WifiSelector::kShiftLocked ? 0x8FA8E0 : kArmedBg;
    }
    if (selector.armedKey() == i) fill = kArmedBg;
    modal.fillRoundRect(k.r.x + 3, k.r.y + 3, k.r.w - 6, k.r.h - 6, 10, kKeyEdge);
    modal.fillRoundRect(k.r.x + 4, k.r.y + 4, k.r.w - 8, k.r.h - 8, 9, fill);
    modal.setFont(isChar ? kPasswordFont : &fonts::lgfxJapanGothic_28);
    modal.setTextDatum(middle_center);
    modal.setTextColor(ink);
    modal.drawString(keyLabel(k), k.r.x + k.r.w / 2, k.r.y + k.r.h / 2);
  }
}

void drawPasswordView() {
  modal.fillScreen(kPaper);
  drawModalHeader(selector.selected().ssid, selector.backArmed());

  const auto &f = WifiSelector::kField;
  modal.drawRoundRect(f.x, f.y, f.w, f.h, 10, kGrey);
  modal.drawRoundRect(f.x + 1, f.y + 1, f.w - 2, f.h - 2, 9, kGrey);
  String shown = selector.password();
  const int len = shown.length();
  if (!selector.showPassword()) {
    shown = "";
    for (int i = 0; i < len; i++) shown += '*';
  }
  modal.setFont(kPasswordFont);
  modal.setTextDatum(middle_left);
  modal.setTextColor(kInk);
  // The end being typed is what matters when it no longer fits.
  while (modal.textWidth(shown) > f.w - 40) shown.remove(0, 1);
  modal.drawString(shown, f.x + 20, f.y + f.h / 2);
  modal.fillRect(f.x + 20 + modal.textWidth(shown) + 2, f.y + 20, 3, f.h - 40, kInk);

  modal.setFont(&fonts::lgfxJapanGothic_28);
  const auto &b = WifiSelector::kShow;
  modal.fillRoundRect(b.x, b.y, b.w, b.h, 10, selector.showArmed() ? kArmedBg : 0xE0E4E0);
  modal.setTextDatum(middle_center);
  modal.drawString(selector.showPassword() ? "Hide" : "Show", b.x + b.w / 2, b.y + b.h / 2);

  modal.setFont(&fonts::lgfxJapanGothic_24);
  modal.setTextDatum(top_left);
  const PasswordCheck check = checkPassword(selector.password());
  if (selector.failed()) {
    modal.setTextColor(kAlertRed);
    modal.drawString("Couldn't connect. Check the password.", 24, 226);
    modal.setTextColor(kGrey);
    modal.drawString("Still using the previous network.", 24, 262);
  } else {
    modal.setTextColor(check == PasswordCheck::kBadHex ? kAlertRed : kGrey);
    modal.drawString(check == PasswordCheck::kBadHex ? "64 characters must all be hex digits" : "8 to 63 characters", 24, 226);
  }
  modal.setTextDatum(top_right);
  modal.setTextColor(kGrey);
  modal.drawString(String(len) + " / 63", 696, 226);
  drawKeyboard();
}

void drawConnectingView() {
  modal.fillScreen(kPaper);
  modal.setFont(&fonts::lgfxJapanGothic_28);
  modal.setTextDatum(middle_center);
  modal.setTextColor(kGrey);
  modal.drawString("Connecting to", kScreenW / 2, 520);
  modal.setFont(&fonts::lgfxJapanGothic_40);
  modal.setTextColor(kInk);
  modal.drawString(fitText(modal, selector.selected().ssid, 660), kScreenW / 2, 590);
}

void drawSelector() {
  switch (selector.view()) {
    case WifiSelector::View::kList: drawListView(); break;
    case WifiSelector::View::kPassword: drawPasswordView(); break;
    case WifiSelector::View::kConnecting: drawConnectingView(); break;
    default: return;
  }
  modal.pushSprite(0, 0);
}

// Passes requests to netTask and its results back.
void pumpWifiExchange() {
  char ssid[33], pass[WifiSelector::kMaxPassword + 1];
  const bool scan = selector.takeScanRequest();
  const bool connect = selector.takeConnectRequest(ssid, pass);
  selectorOpen = selector.visible();
  const bool connected = WiFi.status() == WL_CONNECTED;
  Locked lock;
  selector.setConnectedSsid(connected ? wifiX.currentSsid : "");
  if (scan) wifiX.scanRequested = true;
  if (connect) {
    strcpy(wifiX.connectSsid, ssid);
    strcpy(wifiX.connectPass, pass);
    memset(pass, 0, sizeof pass);
    wifiX.connectRequested = true;
  }
  if (wifiX.scanDone) {
    wifiX.scanDone = false;
    selector.setNetworks(wifiX.scanResults, wifiX.scanCount);
  }
  if (wifiX.connectDone) {
    wifiX.connectDone = false;
    selector.connectFinished(wifiX.connectOk, millis());
  }
}

// --- earthquake pages ---
constexpr int kQuakeInfoY = QuakeBrowser::kHeaderH, kQuakeMapY = 214;
constexpr int kQuakeLegendY = kQuakeMapY + kQuakeMapH;
constexpr int kQuakeAreasY = kQuakeLegendY + 44, kQuakeAreaRowH = 46, kQuakeAreaRows = 5;

// "2026-11-03T14:40:00" -> "2026/11/03 14:40"
String quakeTimeText(const char *t, int precision) {
  if (strlen(t) < 16) return t;
  char out[20];
  if (precision == kQuakeTimeMonth) {
    snprintf(out, sizeof out, "%.4s/%.2s", t, t + 5);
    return out;
  }
  if (precision == kQuakeTimeHour) {
    snprintf(out, sizeof out, "%.4s/%.2s/%.2s %.2s:--", t, t + 5, t + 8, t + 11);
    return out;
  }
  snprintf(out, sizeof out, "%.4s/%.2s/%.2s %.5s", t, t + 5, t + 8, t + 11);
  return out;
}

// When it happened, then how long ago when the clock is known.
String quakeWhenText(const char *t, int precision) {
  String s = quakeTimeText(t, precision);
  const time_t now = clockNow();
  if (now) {
    char ago[24];
    quakeAgoText(t, now, ago, sizeof ago);
    if (ago[0]) s += String("  ") + ago;
  }
  return s;
}

String quakeFactsText(const char *magnitude, int depthKm) {
  String s;
  if (magnitude[0]) s += String("M") + magnitude;
  if (depthKm >= 0) {
    if (s.length()) s += "   ";
    s += String("Depth ") + (depthKm == 0 ? String("shallow") : String(depthKm) + " km");
  }
  return s;
}

// Rounded, as epicentres are only given to 0.1 degree (about 10 km).
String quakeDistanceText(bool hasCoordinate, float lat, float lon) {
  if (!homeSet || !hasCoordinate) return String();
  const int km = (int)(quakeDistanceKm(homeLat, homeLon, lat, lon) + 5) / 10 * 10;
  // In words: this font has no ≈ and draws ~ as a raised bar.
  return km < 10 ? String("under 10 km") : String("about ") + String(km) + " km";
}

// All rows, the live list first.
const QuakeSummary &quakeRow(int row) { return row < quakeLiveCount ? quakeRows[row] : olderRows[row - quakeLiveCount + olderStart]; }
int quakeRowTotal() { return quakeLiveCount + olderCount - olderStart; }
void quakeFindOlderStart() { olderStart = quakeLiveCount > 0 ? quakeFirstOlderThan(olderRows, olderCount, quakeRows[quakeLiveCount - 1].time) : 0; }

// "2026-10-04T20:03:00" -> "2026/10/04"
String quakeDateText(const char *t) {
  if (strlen(t) < 10) return t;
  char out[12];
  snprintf(out, sizeof out, "%.4s/%.2s/%.2s", t, t + 5, t + 8);
  return out;
}

void drawIntensityChip(int x, int y, int size, int rank, bool legacyScale = false) {
  const uint32_t fill = quakeIntensityColor(rank);
  modal.fillRoundRect(x, y, size, size, 10, fill);
  modal.drawRoundRect(x, y, size, size, 10, rank == 1 ? kPanelLine : fill);
  modal.setFont(size >= 80 ? &fonts::lgfxJapanGothic_40 : &fonts::lgfxJapanGothic_24);
  modal.setTextDatum(middle_center);
  modal.setTextColor(quakeIntensityInk(rank));
  modal.drawString(quakeIntensityLabel(rank, legacyScale), x + size / 2, y + size / 2);
}

// Required by JMA's terms: the source, and that the data has been processed.
void drawQuakeCredit() {
  const int y = kScreenH - QuakeBrowser::kFooterH;
  modal.fillRect(0, y, kScreenW, QuakeBrowser::kFooterH, kPaper);
  modal.setFont(&fonts::lgfxJapanGothic_20);
  modal.setTextDatum(middle_center);
  modal.setTextColor(kFaint);
  modal.drawString("出典: 気象庁 (jma.go.jp/bosai/map.html) を加工して作成", kScreenW / 2, y + QuakeBrowser::kFooterH / 2);
}

void drawQuakeHeader(const char *back, const String &title) {
  modal.fillRect(0, 0, kScreenW, QuakeBrowser::kHeaderH, kHeaderBg);
  modal.fillRect(0, QuakeBrowser::kHeaderH - 2, kScreenW, 2, kPanelLine);
  if (browser.backArmed()) modal.fillRect(QuakeBrowser::kBack.x, QuakeBrowser::kBack.y, QuakeBrowser::kBack.w, QuakeBrowser::kBack.h - 2, kArmedBg);
  modal.setFont(&fonts::lgfxJapanGothic_28);
  modal.setTextDatum(middle_left);
  modal.setTextColor(kInk);
  modal.drawString(back, 20, QuakeBrowser::kHeaderH / 2);
  modal.setTextDatum(middle_center);
  modal.setFont(placeNameFont(title.c_str(), &fonts::lgfxJapanGothic_28));
  modal.drawString(fitText(modal, title, 370), kScreenW / 2, QuakeBrowser::kHeaderH / 2);
}

void drawQuakeMessage(const char *msg, bool problem) {
  modal.setFont(&fonts::lgfxJapanGothic_28);
  modal.setTextDatum(middle_center);
  modal.setTextColor(problem ? kAlertRed : kGrey);
  modal.drawString(msg, kScreenW / 2, 560);
}

void drawQuakeFilter() {
  static const char *const labels[] = {"All", "1", "2", "3", "4", "5-", "5+", "6-", "6+", "7"};
  for (int i = 0; i < QuakeBrowser::kFilterCount; i++) {
    const auto r = QuakeBrowser::filterRect(i);
    const bool on = browser.filterOn(i);
    uint32_t fill = kPaper, ink = kGrey;
    if (on) {
      fill = i == 0 ? 0x1E6FD9 : quakeIntensityColor(i);
      ink = i == 0 ? kPaper : quakeIntensityInk(i);
    }
    if (browser.armedFilter() == i) fill = kArmedBg;
    modal.fillRoundRect(r.x + 3, r.y + 8, r.w - 6, r.h - 16, 8, on ? kPanelLine : kKeyEdge);
    modal.fillRoundRect(r.x + 4, r.y + 9, r.w - 8, r.h - 18, 7, fill);
    modal.setFont(&fonts::lgfxJapanGothic_24);
    modal.setTextDatum(middle_center);
    modal.setTextColor(ink);
    modal.drawString(labels[i], r.x + r.w / 2, r.y + r.h / 2);
  }
  modal.fillRect(0, QuakeBrowser::kListTop - 2, kScreenW, 2, kRowLine);
}

// The load control, with a line saying how far back the list reaches.
void drawOlderRow(const QuakeBrowser::Rect &r, int index, const String &reach) {
  const auto load = browser.olderLoad();
  const bool enabled = browser.olderEnabled();
  modal.fillRect(r.x, r.y, r.w, r.h, browser.litRow() == index ? kArmedBg : kHeaderBg);
  modal.setTextDatum(top_center);
  modal.setFont(&fonts::lgfxJapanGothic_28);
  modal.setTextColor(load == QuakeBrowser::Load::kFailed ? kAlertRed : enabled ? kInk : kGrey);
  modal.drawString(load == QuakeBrowser::Load::kLoading ? "Loading older..."
                   : load == QuakeBrowser::Load::kFailed ? "Couldn't load. Tap to retry"
                   : enabled ? "Load older earthquakes"
                   : olderCount + kOlderBatch > kOlderRows ? "The list is full" : "That's all of JMA's records",
                   kScreenW / 2, r.y + 22);
  modal.setFont(&fonts::lgfxJapanGothic_24);
  modal.setTextColor(kGrey);
  modal.drawString(reach, kScreenW / 2, r.y + 66);
}

// Eight dots going round, with the head darkest.
void drawQuakeSpinner(int cx, int cy) {
  static constexpr int kDots = 8, kRadius = 44;
  const int head = (millis() / QuakeBrowser::kSpinMs) % kDots;
  for (int i = 0; i < kDots; i++) {
    const int behind = (head - i + kDots) % kDots;
    const uint32_t shade = 0x44 + behind * 0x1C;
    const float a = i * 2 * M_PI / kDots;
    modal.fillSmoothCircle(cx + (int)(kRadius * sin(a)), cy - (int)(kRadius * cos(a)), 9, (uint32_t)(shade << 16 | shade << 8 | shade));
  }
}

void drawQuakeListPage() {
  modal.fillScreen(kPaper);
  const bool waiting = browser.listWaiting();
  const int count = waiting ? 0 : browser.visibleCount();
  modal.setClipRect(0, QuakeBrowser::kListTop, kScreenW, kScreenH - QuakeBrowser::kListTop - QuakeBrowser::kFooterH);
  for (int i = browser.scrollY() / QuakeBrowser::kRowH; i < count; i++) {
    const auto r = QuakeBrowser::rowRect(i, browser.scrollY());
    if (r.y >= kScreenH) break;
    if (browser.visibleRow(i) < 0) {
      const int rank = history.partialFromRank(browser.filterMask());
      drawOlderRow(r, i, "Before " + quakeDateText(history.coveredSince(browser.filterMask())) + ": " + quakeIntensityLabel(rank, false) +
                             (rank == 9 ? " only" : " and stronger only"));
      continue;
    }
    const QuakeSummary &q = quakeRow(browser.visibleRow(i));
    const uint32_t bg = browser.litRow() == i ? kArmedBg : kPaper;
    if (bg != kPaper) modal.fillRect(r.x, r.y, r.w, r.h, bg);
    modal.fillRect(24, r.y + r.h - 2, kScreenW - 48, 2, kRowLine);
    drawIntensityChip(20, r.y + 16, 84, q.maxRank, q.legacyScale);
    modal.setTextDatum(top_left);
    modal.setTextColor(kInk);
    modal.setFont(placeNameFont(q.name, &fonts::lgfxJapanGothic_28));
    modal.drawString(fitText(modal, q.name[0] ? String(q.name) : String("Epicentre not yet reported"), 580), 124, r.y + 14);
    modal.setFont(&fonts::lgfxJapanGothic_24);
    modal.setTextColor(kGrey);
    modal.drawString(quakeWhenText(q.time, q.timePrecision), 124, r.y + 52);
    modal.drawString(quakeFactsText(q.magnitude, q.depthKm), 124, r.y + 82);
    const String away = quakeDistanceText(q.hasCoordinate, q.lat, q.lon);
    if (away.length()) {
      modal.setTextDatum(top_right);
      modal.setTextColor(kInk);
      modal.drawString(away, 696, r.y + 52);
    }
  }
  const bool haveRows = browser.rowCount() > 0 && !waiting;
  if (haveRows) {
    // Says how far back the list is complete, as "none" only means none that far back.
    const String since = quakeDateText(history.coveredSince(browser.filterMask()));
    drawOlderRow(QuakeBrowser::rowRect(browser.olderRowIndex(), browser.scrollY()), browser.olderRowIndex(), "Complete back to " + since);
    if (count == 0) {
      const auto m = QuakeBrowser::rowRect(0, browser.scrollY());
      modal.setTextDatum(middle_center);
      modal.setFont(&fonts::lgfxJapanGothic_28);
      modal.setTextColor(kGrey);
      modal.drawString("None at these levels since " + since, kScreenW / 2, m.y + m.h / 2);
    }
  }
  modal.clearClipRect();
  if (waiting) {
    drawQuakeSpinner(kScreenW / 2, 460);
    drawQuakeMessage("Updating...", false);
  } else if (!haveRows) {
    if (browser.listLoad() == QuakeBrowser::Load::kFailed) drawQuakeMessage("Couldn't load the list. Retrying...", true);
    else if (browser.listLoad() == QuakeBrowser::Load::kReady) drawQuakeMessage("No earthquakes listed", false);
    else drawQuakeMessage("Loading...", false);
  }
  drawQuakeHeader("< Live", browser.listRefreshFailed() ? "Couldn't refresh" : "Earthquakes");
  drawQuakeFilter();
  drawQuakeCredit();
}

void drawQuakeDetailPage() {
  modal.fillScreen(kPaper);
  const int row = browser.selectedRow();
  const QuakeSummary &q = quakeRow(row);
  // The report's own name once loaded, as a list refresh can move rows underneath this page.
  const bool ready = browser.detailLoad() == QuakeBrowser::Load::kReady;
  const char *title = ready && quakeShown->name[0] ? quakeShown->name : q.name[0] ? q.name : "Earthquake";
  drawQuakeHeader("< List", title);
  drawQuakeCredit();
  if (browser.detailLoad() == QuakeBrowser::Load::kFailed) {
    drawQuakeMessage("Couldn't load this report", true);
    return;
  }
  if (browser.detailLoad() != QuakeBrowser::Load::kReady) {
    drawQuakeMessage("Loading...", false);
    return;
  }
  const QuakeDetail &d = *quakeShown;
  drawIntensityChip(20, kQuakeInfoY + 12, 90, d.maxRank ? d.maxRank : q.maxRank, d.legacyScale);
  modal.setTextDatum(top_left);
  modal.setTextColor(kInk);
  modal.setFont(&fonts::lgfxJapanGothic_24);
  modal.drawString(d.time[0] ? quakeWhenText(d.time, d.timePrecision) : quakeWhenText(q.time, q.timePrecision), 130, kQuakeInfoY + 8);
  {
    String facts = quakeFactsText(d.magnitude, d.depthKm);
    const String away = quakeDistanceText(d.hasCoordinate, d.lat, d.lon);
    if (away.length()) facts += (facts.length() ? "   " : "") + away + " away";
    modal.drawString(facts, 130, kQuakeInfoY + 42);
  }
  modal.setFont(&fonts::lgfxJapanGothic_20);
  modal.setTextColor(kGrey);
  modal.drawString(fitText(modal, d.comment, 570), 130, kQuakeInfoY + 78);

  memcpy(static_cast<uint16_t *>(modal.getBuffer()) + (size_t)kQuakeMapY * kScreenW, quakeMapShown,
         (size_t)kQuakeMapW * kQuakeMapH * 2);

  // The colour key, with the weakest on the left.
  modal.setFont(&fonts::lgfxJapanGothic_20);
  modal.setTextDatum(middle_center);
  for (int rank = 1; rank <= 9; rank++) {
    const int w = kScreenW / 9, x = (rank - 1) * w;
    modal.fillRect(x, kQuakeLegendY, w, 38, quakeIntensityColor(rank));
    modal.setTextColor(quakeIntensityInk(rank));
    modal.drawString(quakeIntensityLabel(rank), x + w / 2, kQuakeLegendY + 19);
  }

  for (int i = 0; i < d.areaCount && i < kQuakeAreaRows; i++) {
    const int y = kQuakeAreasY + i * kQuakeAreaRowH;
    drawIntensityChip(20, y + 2, 42, d.areas[i].rank, d.legacyScale);
    modal.setFont(placeNameFont(d.areas[i].name, &fonts::lgfxJapanGothic_24));
    modal.setTextDatum(middle_left);
    modal.setTextColor(kInk);
    modal.drawString(fitText(modal, d.areas[i].name, 480), 76, y + kQuakeAreaRowH / 2);
  }
  if (d.areaCount > kQuakeAreaRows) {
    modal.setFont(&fonts::lgfxJapanGothic_20);
    modal.setTextDatum(bottom_right);
    modal.setTextColor(kGrey);
    modal.drawString("+" + String((d.areasAreStations ? d.stationCount : d.areaCount) - kQuakeAreaRows) + (d.areasAreStations ? " more stations" : " more areas"), 700, kQuakeAreasY + kQuakeAreaRows * kQuakeAreaRowH);
  }
}

void composeQuakePage() {
  if (browser.view() == QuakeBrowser::View::kList) drawQuakeListPage();
  else if (browser.view() == QuakeBrowser::View::kDetail) drawQuakeDetailPage();
}

void drawQuakePage() {
  if (!browser.visible()) return;
  composeQuakePage();
  modal.pushSprite(0, 0);
}

// The live page and the list side by side, moved left by `pos`.
void showPageSlide(int pos) {
  M5.Display.startWrite();
  canvas.pushSprite(-pos, 0);
  modal.pushSprite(kScreenW - pos, 0);
  M5.Display.endWrite();
}

const char *quakeRowJson(int row) { return quakeRow(row).json; }
int quakeRowRank(int row) { return quakeRow(row).maxRank; }
bool quakeRowPartial(int row) { return strcmp(quakeRow(row).time, history.coveredSince(browser.filterMask())) < 0; }

// --- location screen ---
void drawLocationScreen() {
  modal.fillScreen(kPaper);
  modal.fillRect(0, 0, kScreenW, 100, kHeaderBg);
  modal.fillRect(0, 98, kScreenW, 2, kPanelLine);
  if (locationEditor.backArmed()) modal.fillRect(0, 0, 150, 98, kArmedBg);
  modal.setFont(&fonts::lgfxJapanGothic_28);
  modal.setTextDatum(middle_left);
  modal.setTextColor(kInk);
  modal.drawString("< Back", 20, 50);
  modal.setTextDatum(middle_center);
  modal.drawString("Location", kScreenW / 2, 50);

  static const char *const titles[] = {"Latitude (north is positive)", "Longitude (east is positive)"};
  for (int f = 0; f < 2; f++) {
    const auto r = LocationEditor::fieldRect(f);
    const bool active = locationEditor.active() == f;
    const char *t = locationEditor.text(f);
    modal.setFont(&fonts::lgfxJapanGothic_24);
    modal.setTextDatum(bottom_left);
    modal.setTextColor(kGrey);
    modal.drawString(titles[f], r.x, r.y - 8);
    const uint32_t edge = t[0] && !locationEditor.valid(f) ? kAlertRed : active ? 0x1E6FD9 : kKeyEdge;
    modal.fillRoundRect(r.x, r.y, r.w, r.h, 10, edge);
    modal.fillRoundRect(r.x + 3, r.y + 3, r.w - 6, r.h - 6, 8, kPaper);
    modal.setFont(&fonts::FreeSansBold24pt7b);
    modal.setTextDatum(middle_left);
    modal.setTextColor(kInk);
    modal.drawString(t, r.x + 20, r.y + r.h / 2);
    if (active) modal.fillRect(r.x + 24 + modal.textWidth(t), r.y + 20, 3, r.h - 40, kInk);
  }
  modal.setFont(&fonts::lgfxJapanGothic_20);
  modal.setTextDatum(top_left);
  modal.setTextColor(kGrey);
  modal.drawString("Stays on this device. Only used to work out distances.", 20, 470);

  LocationEditor::Key keys[LocationEditor::kKeyCount];
  const int n = LocationEditor::keys(keys);
  for (int i = 0; i < n; i++) {
    const auto &k = keys[i];
    const bool save = k.kind == LocationEditor::kSave;
    const bool enabled = !save || locationEditor.canSave();
    uint32_t fill = k.kind == LocationEditor::kDigit || k.kind == LocationEditor::kPoint ? kPaper : 0xE0E4E0;
    uint32_t ink = enabled ? kInk : 0xB0B4B0;
    if (save && enabled) {
      fill = 0x1E6FD9;
      ink = kPaper;
    }
    if (locationEditor.armedKey() == i) fill = kArmedBg;
    modal.fillRoundRect(k.r.x + 3, k.r.y + 3, k.r.w - 6, k.r.h - 6, 10, kKeyEdge);
    modal.fillRoundRect(k.r.x + 4, k.r.y + 4, k.r.w - 8, k.r.h - 8, 9, fill);
    modal.setFont(k.kind == LocationEditor::kDigit || k.kind == LocationEditor::kPoint || k.kind == LocationEditor::kMinus ? &fonts::lgfxJapanGothic_40 : &fonts::lgfxJapanGothic_28);
    modal.setTextDatum(middle_center);
    modal.setTextColor(ink);
    if (k.kind == LocationEditor::kDigit) {
      const char label[2] = {k.ch, 0};
      modal.drawString(label, k.r.x + k.r.w / 2, k.r.y + k.r.h / 2);
    } else {
      modal.drawString(k.label, k.r.x + k.r.w / 2, k.r.y + k.r.h / 2);
    }
  }
  modal.pushSprite(0, 0);
}

void drawNightScreen() {
  modal.fillScreen(kPaper);
  modal.fillRect(0, 0, kScreenW, 100, kHeaderBg);
  modal.fillRect(0, 98, kScreenW, 2, kPanelLine);
  if (nightEditor.backArmed()) modal.fillRect(0, 0, 150, 98, kArmedBg);
  modal.setFont(&fonts::lgfxJapanGothic_28);
  modal.setTextDatum(middle_left);
  modal.setTextColor(kInk);
  modal.drawString("< Back", 20, 50);
  modal.setTextDatum(middle_center);
  modal.drawString("Night mode", kScreenW / 2, 50);

  static const char *const modes[] = {"Off", "Scheduled", "Continuous"};
  for (int m = 0; m < 3; m++) {
    const auto r = NightModeEditor::modeRect(m);
    const bool on = nightEditor.mode() == m;
    const uint32_t fill = nightEditor.armedMode() == m ? kArmedBg : on ? 0x1E6FD9 : kPaper;
    modal.fillRoundRect(r.x, r.y, r.w, r.h, 10, on ? 0x1E6FD9 : kKeyEdge);
    modal.fillRoundRect(r.x + 2, r.y + 2, r.w - 4, r.h - 4, 8, fill);
    modal.setFont(&fonts::lgfxJapanGothic_28);
    modal.setTextDatum(middle_center);
    modal.setTextColor(on && nightEditor.armedMode() != m ? kPaper : kInk);
    modal.drawString(modes[m], r.x + r.w / 2, r.y + r.h / 2);
  }

  modal.setFont(&fonts::lgfxJapanGothic_24);
  modal.setTextDatum(middle_left);
  modal.setTextColor(kGrey);
  modal.drawString("On after a touch", 20, 445);
  for (int plus = 0; plus < 2; plus++) {
    const auto r = NightModeEditor::wakeRect(plus);
    const bool enabled = nightEditor.canWake(plus);
    const uint32_t mark = enabled ? kInk : 0xB0B4B0;
    modal.fillRoundRect(r.x, r.y, r.w, r.h, 10, enabled ? kKeyEdge : 0xE0E4E0);
    modal.fillRoundRect(r.x + 2, r.y + 2, r.w - 4, r.h - 4, 8, nightEditor.armedWake() == plus ? kArmedBg : kPaper);
    const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    modal.fillRect(cx - 20, cy - 3, 40, 6, mark);
    if (plus) modal.fillRect(cx - 3, cy - 20, 6, 40, mark);
  }
  modal.setFont(&fonts::FreeSansBold24pt7b);
  modal.setTextDatum(middle_center);
  modal.setTextColor(kInk);
  modal.drawString(String(nightEditor.wakeSeconds()) + " s", 490, 445);

  const bool scheduled = nightEditor.scheduled();
  static const char *const titles[] = {"Off from", "On again at"};
  for (int f = 0; f < 2; f++) {
    const auto r = NightModeEditor::fieldRect(f);
    const bool wrong = scheduled && (!nightEditor.complete(f) || nightEditor.timesEqual());
    const uint32_t edge = !scheduled ? kKeyEdge : wrong && nightEditor.active() != f ? kAlertRed : nightEditor.active() == f ? 0x1E6FD9 : kKeyEdge;
    modal.setFont(&fonts::lgfxJapanGothic_24);
    modal.setTextDatum(bottom_left);
    modal.setTextColor(scheduled ? kGrey : 0xB0B4B0);
    modal.drawString(titles[f], r.x, r.y - 8);
    modal.fillRoundRect(r.x, r.y, r.w, r.h, 10, edge);
    modal.fillRoundRect(r.x + 3, r.y + 3, r.w - 6, r.h - 6, 8, kPaper);
    char t[6];
    nightEditor.display(f, t);
    modal.setFont(&fonts::FreeSansBold24pt7b);
    modal.setTextDatum(middle_center);
    modal.setTextColor(scheduled ? kInk : 0xB0B4B0);
    modal.drawString(t, r.x + r.w / 2, r.y + r.h / 2);
  }
  modal.setFont(&fonts::lgfxJapanGothic_20);
  modal.setTextDatum(top_left);
  modal.setTextColor(kGrey);
  if (scheduled && nightEditor.timesEqual()) {
    modal.setTextColor(kAlertRed);
    modal.drawString("The two times must be different.", 20, 505);
  } else if (nightEditor.mode() == NightSchedule::kOff) {
    modal.drawString("The screen stays on, dimming at night.", 20, 505);
  } else {
    modal.drawString(scheduled ? "24-hour clock, Japan time. The screen switches off in this time." : "The screen stays off, whatever the time.", 20, 505);
    modal.drawString("A touch wakes it. An earthquake alert wakes it", 20, 540);
    modal.drawString("and sounds the alarm at the Sound setting.", 20, 570);
  }

  NightModeEditor::Key keys[NightModeEditor::kKeyCount];
  const int n = NightModeEditor::keys(keys);
  for (int i = 0; i < n; i++) {
    const auto &k = keys[i];
    const bool enabled = nightEditor.keyEnabled(k);
    const bool save = k.kind == NightModeEditor::kSave;
    uint32_t fill = k.kind == NightModeEditor::kDigit ? kPaper : 0xE0E4E0;
    uint32_t ink = enabled ? kInk : 0xB0B4B0;
    if (save && enabled) {
      fill = 0x1E6FD9;
      ink = kPaper;
    }
    if (nightEditor.armedKey() == i) fill = kArmedBg;
    modal.fillRoundRect(k.r.x + 3, k.r.y + 3, k.r.w - 6, k.r.h - 6, 10, kKeyEdge);
    modal.fillRoundRect(k.r.x + 4, k.r.y + 4, k.r.w - 8, k.r.h - 8, 9, fill);
    modal.setFont(k.kind == NightModeEditor::kDigit ? &fonts::lgfxJapanGothic_40 : &fonts::lgfxJapanGothic_28);
    modal.setTextDatum(middle_center);
    modal.setTextColor(ink);
    if (k.kind == NightModeEditor::kDigit) {
      const char label[2] = {k.ch, 0};
      modal.drawString(label, k.r.x + k.r.w / 2, k.r.y + k.r.h / 2);
    } else {
      modal.drawString(k.label, k.r.x + k.r.w / 2, k.r.y + k.r.h / 2);
    }
  }
  modal.pushSprite(0, 0);
}

// Passes requests to quakeTask and takes its finished results.
void pumpQuakeExchange() {
  char json[QuakeBrowser::kJsonNameSize];
  int row;
  const bool list = browser.takeListRequest();
  const bool detail = browser.takeDetailRequest(json, &row);
  const bool older = browser.takeOlderRequest();
  const bool roomForMore = olderCount + kOlderBatch <= kOlderRows;
  browser.setOlderEnabled(history.canLoadOlder(browser.filterMask()) && roomForMore);
  // New rows for the built-in levels come by themselves, so they stay complete.
  const uint32_t nowMs = millis();
  const bool topUp = !older && roomForMore && history.needsTopUp() && browser.olderLoad() != QuakeBrowser::Load::kLoading &&
                     (lastTopUpMs == 0 || nowMs - lastTopUpMs > kTopUpRetryMs || browser.olderLoad() == QuakeBrowser::Load::kReady);
  if (topUp) {
    lastTopUpMs = nowMs;
    browser.olderStarted();
  }
  Locked lock;
  if (list) quakeX.listRequested = true;
  if (older || topUp) {
    olderPlanSent = older ? history.plan(browser.filterMask()) : history.plan(1u << QuakeHistory::kPreloadRank);
    quakeX.olderPlan = olderPlanSent;
    quakeX.olderRequested = true;
  }
  if (quakeX.olderDone) {
    quakeX.olderDone = false;
    const int got = quakeX.olderCount;
    if (got >= 0 && history.applyResult(olderPlanSent, got > 0 ? quakeX.olderBatch[got - 1].time : nullptr, got >= QuakeHistory::kDbRowLimit)) {
      olderCount = quakeMergeOlder(olderRows, olderCount, kOlderRows, quakeX.olderBatch, got);
      quakeFindOlderStart();
    }
    browser.olderLoaded(got >= 0, quakeRowTotal());
  }
  if (detail) {
    strcpy(quakeX.detailJson, json);
    quakeX.detailRequested = true;
  }
  if (quakeX.listDone) {
    quakeX.listDone = false;
    if (quakeX.listOk) {
      QuakeSummary *old = quakeRows;
      quakeRows = quakeX.list;
      quakeX.list = old;
      quakeLiveCount = quakeX.listCount;
      if (quakeLiveCount > 0 && history.liveLoaded(quakeRows[quakeLiveCount - 1].time, quakeRows[0].time)) {
        // The live list has moved on past the weak rows, which would leave a hole.
        olderCount = quakeKeepFromRank(olderRows, olderCount, QuakeHistory::kPreloadRank);
      }
      quakeFindOlderStart();
    }
    browser.listLoaded(quakeX.listOk, quakeRowTotal(), millis());
  }
  if (quakeX.detailDone) {
    quakeX.detailDone = false;
    // A slower answer to an earlier tap isn't what's on screen now.
    if (strcmp(quakeX.detailFor, browser.selectedJson()) == 0) {
      if (quakeX.detailOk) {
        QuakeDetail *oldDetail = quakeShown;
        quakeShown = quakeX.detail;
        quakeX.detail = oldDetail;
        uint16_t *oldMap = quakeMapShown;
        quakeMapShown = quakeX.map;
        quakeX.map = oldMap;
      }
      browser.detailLoaded(quakeX.detailOk);
    }
  }
}

struct PsramJsonAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
PsramJsonAllocator psramJson;

// Collects a response body, chunked or not.
class DownloadSink : public Stream {
 public:
  size_t len = 0;
  bool overflow = false;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *data, size_t n) override {
    if (len + n > kQuakeDownloadCap) {
      overflow = true;
      return 0;
    }
    memcpy(quakeDownload + len, data, n);
    len += n;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
};

// GET, or POST if there's a body. Both JMA hosts share a certificate authority.
bool jmaFetch(const String &url, const String *body, size_t *outLen) {
  if (WiFi.status() != WL_CONNECTED) return false;
  const uint32_t t0 = millis();
  NetworkClientSecure tls;
  tls.setCACert(kJmaRootCa);
  HTTPClient client;
  client.setReuse(true);  // asking to close can end a chunked reply before its last marker is read
  client.setConnectTimeout(8000);
  client.setTimeout(10000);
  if (!client.begin(tls, url)) return false;
  if (body) client.addHeader("Content-Type", "application/x-www-form-urlencoded");
  const int status = body ? client.POST(*body) : client.GET();
  DownloadSink sink;
  int written = -1;
  if (status == 200) written = client.writeToStream(&sink);
  client.end();
  tls.stop();
  // The body's own format shows if it's complete: a cut-off reply won't parse.
  const bool ok = status == 200 && sink.len > 0 && !sink.overflow;
  taskLog("jma %s: http=%d written=%d bytes=%u %ums minheap=%u\n", url.c_str() + 8, status, written, (unsigned)sink.len,
                (unsigned)(millis() - t0), (unsigned)ESP.getMinFreeHeap());
  if (!ok) {
    quakeFetchFailures = quakeFetchFailures + 1;
    return false;
  }
  *outLen = sink.len;
  return true;
}

bool jmaGet(const String &path, size_t *outLen) {
  return jmaFetch("https://www.jma.go.jp/bosai/" + path, nullptr, outLen);
}

const char *const kJmaDb = "https://www.data.jma.go.jp/eqdb/data/shindo/api/";

void *quakePsramAlloc(size_t n) { return psramAlloc<uint8_t>(n); }

bool loadQuakeRegions() {
  if (quakeRegions.loaded()) return true;
  size_t len = 0;
  if (!jmaGet("common/const/geojson/earthquake.json", &len)) return false;
  const uint32_t t0 = millis();
  const bool ok = quakeLoadRegions(reinterpret_cast<const char *>(quakeDownload), len, &quakeRegions, quakePsramAlloc, &psramJson);
  taskLog("regions %s: %d regions %d points, %ums\n", ok ? "ok" : "failed", quakeRegions.regionCount, quakeRegions.pointCount, (unsigned)(millis() - t0));
  return ok;
}

void serviceQuakeList() {
  QuakeSummary *target;
  {
    Locked lock;
    if (!quakeX.listRequested || quakeX.listDone) return;
    quakeX.listRequested = false;
    target = quakeX.list;
  }
  size_t len = 0;
  int rows = -1;
  if (jmaGet("quake/data/list.json", &len)) {
    const uint32_t t0 = millis();
    rows = quakeParseList(reinterpret_cast<const char *>(quakeDownload), len, target, kQuakeRows, &psramJson);
    taskLog("list parsed: %d rows, %ums\n", rows, (unsigned)(millis() - t0));
  }
  Locked lock;
  quakeX.listOk = rows >= 0;
  quakeX.listCount = rows < 0 ? 0 : rows;
  quakeX.listDone = true;
}

void serviceQuakeDetail() {
  QuakeDetail *target;
  uint16_t *map;
  char json[QuakeBrowser::kJsonNameSize];
  {
    Locked lock;
    if (!quakeX.detailRequested || quakeX.detailDone) return;
    quakeX.detailRequested = false;
    strcpy(json, quakeX.detailJson);
    target = quakeX.detail;
    map = quakeX.map;
  }
  bool ok = loadQuakeRegions();
  size_t len = 0;
  const bool fromDb = strncmp(json, "db:", 3) == 0;
  if (ok && fromDb) {
    const String body = String("mode=event&id=") + (json + 3);
    ok = jmaFetch(kJmaDb, &body, &len);
  } else if (ok) {
    ok = jmaGet(String("quake/data/") + json, &len);
  }
  if (ok) {
    const uint32_t t0 = millis();
    ok = fromDb ? quakeParseDbEvent(reinterpret_cast<const char *>(quakeDownload), len, target, &psramJson)
                : quakeParseDetail(reinterpret_cast<const char *>(quakeDownload), len, target, &psramJson);
    const uint32_t t1 = millis();
    if (ok) quakeRenderMap(quakeRegions, *target, map, kQuakeMapW, kQuakeMapH);
    taskLog("detail %s: %d stations %d areas, parse %ums render %ums\n", ok ? "ok" : "failed", target->stationCount,
                  target->areaCount, (unsigned)(t1 - t0), (unsigned)(millis() - t1));
  }
  Locked lock;
  strcpy(quakeX.detailFor, json);
  quakeX.detailOk = ok;
  quakeX.detailDone = true;
}

void serviceQuakeOlder() {
  QuakeDbRequest q;
  QuakeSummary *target;
  {
    Locked lock;
    if (!quakeX.olderRequested || quakeX.olderDone) return;
    quakeX.olderRequested = false;
    q = quakeX.olderPlan;
    target = quakeX.olderBatch;
  }
  // No limits on magnitude, depth or place; the form's own "any" values.
  String body = "mode=search&dateTimeF[]=" + String(q.fromDate) + "&dateTimeF[]=00:00&dateTimeT[]=" + q.toDate +
                "&dateTimeT[]=" + q.toTime + "&mag[]=0.0&mag[]=9.9&dep[]=0&dep[]=999&epi[]=99&pref[]=99&city[]=99&station[]=99"
                "&obsInt=1&maxInt=" + String(QuakeHistory::dbLevelCode(q.minRank)) +
                "&additionalC=true&Sort=S0&Comp=C0&seisCount=false&observed=false";
  size_t len = 0;
  int rows = -1;
  if (jmaFetch(kJmaDb, &body, &len)) {
    const uint32_t t0 = millis();
    rows = quakeParseDbSearch(reinterpret_cast<const char *>(quakeDownload), len, target, kOlderBatch, &psramJson);
    taskLog("older parsed: %d rows %s to %s from level %d, %ums\n", rows, q.fromDate, q.toDate, q.minRank, (unsigned)(millis() - t0));
  }
  Locked lock;
  quakeX.olderCount = rows;
  quakeX.olderDone = true;
}

void quakeTask(void *) {
  for (;;) {
    serviceQuakeList();
    serviceQuakeOlder();
    serviceQuakeDetail();
    delay(50);
  }
}

QuakeDetail *newQuakeDetail() {
  QuakeDetail *d = psramAlloc<QuakeDetail>(1);
  memset(d, 0, sizeof(QuakeDetail));
  d->stations = psramAlloc<QuakeStation>(kQuakeStationCap);
  d->stationCap = kQuakeStationCap;
  d->areas = psramAlloc<QuakeArea>(kQuakeAreaCap);
  d->areaCap = kQuakeAreaCap;
  return d;
}

bool loadStaticLayers() {
  for (int i = 0; i < kSrcW * kSrcH; i++) baseLayer[i] = 0xFFFFFFFFu;  // the page's white map background
  if (!fetchLayer("/data/map_img/CommonImg/base_map_w.gif", baseLayer, kSrcW, kSrcH)) return false;

  uint32_t *raw = psramAlloc<uint32_t>(kScaleSrcW * kScaleSrcH);
  memset(raw, 0, kScaleSrcW * kScaleSrcH * sizeof(uint32_t));
  bool ok = fetchLayer("/data/map_img/ScaleImg/nied_jma_s_w_scale.gif", raw, kScaleSrcW, kScaleSrcH);
  if (ok) {
    static ResampleAxis ax[kScaleW], ay[kScaleH];
    resampleAxis(ax, kScaleSrcW, kScaleW);
    resampleAxis(ay, kScaleSrcH, kScaleH);
    resampleRgba(raw, kScaleSrcW, ax, kScaleW, ay, kScaleH, scaleLayer);
  }
  free(raw);
  return ok;
}

// Downloaded rather than embedded, as NIED's content isn't ours to redistribute.
void loadAlarmSounds() {
  lastAlarmLoadMs = millis();
  for (Wav *w : {&wavWarning, &wavForecast}) {
    if (w->data) continue;
    size_t len = 0;
    if (httpGet(w->path, &len) != 200 || len < 44 || memcmp(download, "RIFF", 4) != 0) {
      taskLog("alarm sound %s failed\n", w->path);
      continue;
    }
    uint8_t *copy = psramAlloc<uint8_t>(len);
    memcpy(copy, download, len);
    w->len = len;
    w->data = copy;  // loop() only reads this after seeing a non-null pointer
    taskLog("alarm sound %s ok, %u bytes\n", w->path, (unsigned)len);
  }
}

// Reads the battery when the gauge asks. Runs here because the sensor shares a bus with touch.
void updateBattery() {
  const uint32_t now = millis();
  const auto action = battery.next(now);
  if (action == BatteryGauge::Action::kNone) return;
  if (action == BatteryGauge::Action::kPauseCharging) {
    M5.Power.setBatteryCharge(false);
    return;
  }
  const float mv = M5.Power.getBatteryVoltage() / 2.0f;  // the Tab5's pack has two cells
  const float mA = M5.Power.getBatteryCurrent();
  const bool chargerOn = M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
  const bool duringPause = action == BatteryGauge::Action::kReadThenResume;
  battery.reading(now, mv, mA, chargerOn, duringPause);
  if (duringPause) M5.Power.setBatteryCharge(true);
}

void playPendingAlarm() {
  Shared::Alarm alarm;
  {
    Locked lock;
    alarm = shared.alarm;
    shared.alarm = Shared::kNoAlarm;
  }
  if (alarm == Shared::kNoAlarm) return;
  alarmsPlayed++;
  lastAlarmMs = millis();
  selector.close();  // an alert always brings the map back
  browser.close();
  locationEditor.close();
  nightEditor.close();
  if (menu.sound() == 0) return;
  const Wav &w = alarm == Shared::kAlarmWarning ? wavWarning : wavForecast;
  // If the download hasn't succeeded yet, a plain tone is better than silence.
  if (!w.data || !M5.Speaker.playWav(w.data, w.len, 1, -1, true)) {
    M5.Speaker.tone(alarm == Shared::kAlarmWarning ? 2000 : 1000, 1000);
  }
}

void handleSerial() {
  if (!Serial.available()) return;
  const char c = Serial.read();
  if (c == 'S') {
    serialDumping = true;
    delay(100);
    Serial.setTxTimeoutMs(2000);  // first: flushing with no timeout can read as a lost host
    Serial.flush();
    Serial.printf("\nKMONI-SCREEN %d %d\n", kScreenW, kScreenH);
    static lgfx::swap565_t row[kScreenW];
    for (int y = 0; y < kScreenH; y++) {
      M5.Display.readRect(0, y, kScreenW, 1, row);
      Serial.write(reinterpret_cast<const uint8_t *>(row), sizeof row);
    }
    Serial.flush();
    Serial.setTxTimeoutMs(0);
    serialDumping = false;
  } else if (c == 'R') {
    char digits[15] = {};
    Serial.setTimeout(200);
    Serial.readBytes(digits, 14);
    const time_t t = parseStamp(digits);
    if (t) {
      Locked lock;
      shared.replayTarget = t;
      shared.replayRequested = true;
      Serial.printf("replay from %s\n", digits);
    }
  } else if (c == 'L') {
    Locked lock;
    shared.replayTarget = 0;
    shared.replayRequested = true;
    Serial.println("live");
  } else if (c == 'I') {
    uint32_t age;
    {
      Locked lock;
      age = millis() - shared.lastFrameMs;
    }
    Serial.setTxTimeoutMs(100);  // this line outgrows the buffer; someone is listening if asked
    Serial.printf("board=%d display=%dx%d rotation=%d uptime=%us shown=%u fellback=%u failed=%u age=%ums rssi=%d wifi=%d heap=%u psram=%u "
                  "brightness=%d level=%d sound=%d volume=%d menu=%d touches=%u alarms=%u playing=%d uiworst=%ums "
                  "view=%d nets=%d scanning=%d failed=%d page=%d pwlen=%d wifisrc=%s known=%d "
                  "qwait=%d night=%d nightstart=%d nightend=%d nightwake=%d backlightoff=%d nightview=%d dimmin=%d sunrise=%d sunset=%d qdiv=%d qlit=%d locview=%d home=%d homelat=%.4f homelon=%.4f older=%d qolder=%d since=%s qslide=%d qview=%d qrows=%d qvis=%d qselected=%d qscroll=%d qfilter=%u qlist=%d qdetail=%d qfail=%u auto=%d minheap=%u ssid=%s\n",
                  (int)M5.getBoard(), M5.Display.width(), M5.Display.height(), M5.Display.getRotation(),
                  (unsigned)(millis() / 1000), (unsigned)framesShown, (unsigned)framesFellBack,
                  (unsigned)framesFailed, (unsigned)age, WiFi.RSSI(), (int)WiFi.status(),
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(), menu.brightness(),
                  (int)M5.Display.getBrightness(), menu.sound(), (int)M5.Speaker.getVolume(), menu.position(),
                  (unsigned)touchesSeen, (unsigned)alarmsPlayed, (int)M5.Speaker.isPlaying(), (unsigned)uiWorstMs,
                  (int)selector.view(), selector.networkCount(), (int)selector.scanning(), (int)selector.failed(),
                  (int)selector.page(), (int)strlen(selector.password()), wifiFromNvs ? "nvs" : "secrets", known.count(),
                  (int)browser.listWaiting(), nightMode.mode, nightMode.start, nightMode.end, nightMode.wakeSeconds, (int)backlightOff, (int)nightEditor.visible(), dimMinute, sunToday.rise, sunToday.set, browser.dividerIndex(), browser.litRow(), (int)locationEditor.visible(), (int)homeSet, homeLat, homeLon, olderCount - olderStart, (int)browser.olderLoad(), history.coveredSince(browser.filterMask()),
                  pageSlide.active() ? pageSlide.position() : -1, (int)browser.view(), browser.rowCount(), browser.visibleCount(), browser.selectedRow(), browser.scrollY(), browser.filterMask(),
                  (int)browser.listLoad(), (int)browser.detailLoad(),
                  (unsigned)quakeFetchFailures, menu.autoReturnSeconds(), (unsigned)ESP.getMinFreeHeap(),
                  currentSsid().c_str());
    Serial.setTxTimeoutMs(0);
    uiWorstMs = 0;
  } else if (c == 'N') {
    // N<hhmm> acts as if it were that time; anything else returns to the clock.
    char digits[5] = {};
    Serial.setTimeout(200);
    Serial.readBytes(digits, 4);
    int h, m;
    forcedMinute = sscanf(digits, "%2d%2d", &h, &m) == 2 ? h * 60 + m : -1;
    Serial.printf("dim minute %d\n", forcedMinute);
  } else if (c == 'X') {
    ESP.restart();
  } else if (c == 'D') {
    Serial.println("dropping Wi-Fi");
    dropWifiRequested = true;
  } else if (c == 'P') {
    char digits[9] = {};
    Serial.setTimeout(200);
    if (Serial.readBytes(digits, 8) == 8) {
      injectY = atoi(digits + 4);
      digits[4] = 0;
      injectX = atoi(digits);
      injectActive = injectPressed = true;
    }
  } else if (c == 'U') {
    injectPressed = false;
  } else if (c == 'H') {
    prefs.putString("ssid", WIFI_SSID);
    prefs.putString("pass", WIFI_PASS);
    Serial.println("last network set, takes effect on restart");
  } else if (c == 'W') {
    for (int i = 0; i < selector.networkCount(); i++) {
      const auto &n = selector.network(i);
      Serial.printf("net %d %d %d %d %s\n", i, n.rssi, (int)n.security, (int)n.known, n.ssid);
    }
    Serial.println("net end");
  } else if (c == 'B') {
    const float mv = M5.Power.getBatteryVoltage() / 2.0f;
    Serial.printf("battery raw %.0fmV/cell %dmA chargerOn=%d shown=%d ext=%d charging=%d known=%d present=%d\n", mv, (int)M5.Power.getBatteryCurrent(),
                  (int)(M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging), battery.level(), (int)battery.external(),
                  (int)battery.charging(), (int)battery.known(), (int)battery.present());
  } else if (c == 'Z') {
    // How many rows the list holds at each level: too many to print them all.
    int counts[10] = {};
    for (int i = 0; i < browser.rowCount(); i++) counts[quakeRow(i).maxRank]++;
    Serial.print("ranks");
    for (int r = 1; r <= 9; r++) Serial.printf(" %d", counts[r]);
    Serial.println();
  } else if (c == 'Q') {
    // The live list; 'Z' covers the rest.
    for (int i = 0; i < quakeLiveCount; i++) {
      Serial.setTxTimeoutMs(100);
      Serial.printf("row %d %d %d %s\n", i, (int)quakeRow(i).maxRank, (int)quakeRow(i).jsonHasStations, quakeRow(i).name);
    }
    Serial.println("row end");
    Serial.setTxTimeoutMs(0);
  } else if (c == 'K') {
    WifiSelector::Key keys[WifiSelector::kMaxKeys];
    const int n = selector.keys(keys);
    for (int i = 0; i < n; i++) {
      Serial.printf("key %d %d %d %d %d %d\n", (int)keys[i].kind, (int)keys[i].ch, keys[i].r.x, keys[i].r.y, keys[i].r.w, keys[i].r.h);
    }
    Serial.println("key end");
  }
}

void joinKnownNetwork();

void keepWifiAlive() {
  if (dropWifiRequested) {
    dropWifiRequested = false;
    WiFi.disconnect();
  }
  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    wifiDownSinceMs = 0;
    return;
  }
  if (!wifiDownSinceMs) wifiDownSinceMs = now;
  const uint32_t patience = firstJoinTried ? kWifiKickAfterMs : kFirstJoinAfterMs;
  if (!selectorOpen && now - wifiDownSinceMs > patience) {
    firstJoinTried = true;
    joinKnownNetwork();
    wifiDownSinceMs = millis();
  }
}

// WPA2 and WPA3 personal only; these are what WiFi.begin() with a passphrase can join.
WifiSelector::Security securityOf(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return WifiSelector::kOpen;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK: return WifiSelector::kPsk;
    default: return WifiSelector::kUnsupported;
  }
}

void beginScan() {
  scanRetried = false;
  scanRunning = true;
  // A radio still trying to connect refuses to scan.
  if (WiFi.status() != WL_CONNECTED) WiFi.disconnect();
  if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) {
    delay(300);
    WiFi.scanNetworks(true);
  }
}

// Results come strongest first; an SSID with several access points appears once.
void pollScan() {
  const int16_t found = WiFi.scanComplete();
  if (found == WIFI_SCAN_RUNNING) return;
  if (found == WIFI_SCAN_FAILED && !scanRetried) {
    scanRetried = true;
    delay(500);
    if (WiFi.status() != WL_CONNECTED) WiFi.disconnect();
    WiFi.scanNetworks(true);
    return;
  }
  scanRunning = false;
  static WifiSelector::Network list[WifiSelector::kMaxNetworks];
  int count = 0;
  for (int i = 0; i < found; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty() || ssid.length() > 32) continue;
    const int rssi = WiFi.RSSI(i);
    int at = -1;
    for (int j = 0; j < count; j++) {
      if (ssid == list[j].ssid) at = j;
    }
    if (at >= 0) continue;  // the strongest was already kept
    if (count == WifiSelector::kMaxNetworks) continue;
    strcpy(list[count].ssid, ssid.c_str());
    list[count].rssi = rssi;
    list[count].security = securityOf(WiFi.encryptionType(i));
    list[count].known = known.find(list[count].ssid) != nullptr;
    count++;
  }
  // Sort, in case the driver didn't.
  for (int i = 1; i < count; i++) {
    const WifiSelector::Network n = list[i];
    int j = i - 1;
    while (j >= 0 && list[j].rssi < n.rssi) {
      list[j + 1] = list[j];
      j--;
    }
    list[j + 1] = n;
  }
  WiFi.scanDelete();
  taskLog("scan: %d found, %d listed\n", (int)found, count);
  Locked lock;
  memcpy(wifiX.scanResults, list, sizeof(WifiSelector::Network) * count);
  wifiX.scanCount = count;
  wifiX.scanDone = true;
}

// Leaves the current network and waits to join this one. The first try at a saved network can
// be refused and the next one accepted, so those get several `attempts`; a typed password gets
// one, as repeating a wrong password at a router gains nothing.
bool tryNetwork(const char *ssid, const char *pass, bool giveWayToSelector, int attempts) {
  taskLog("connecting to %s\n", ssid);
  http.end();
  tcp.stop();
  const uint32_t started = millis();
  for (int attempt = 0; attempt < attempts; attempt++) {
    WiFi.disconnect();
    delay(200);
    const uint32_t eventsBefore = disconnectEvents;
    WiFi.begin(ssid, pass[0] ? pass : nullptr);
    while (WiFi.status() != WL_CONNECTED && millis() - started < kConnectTimeoutTotalMs) {
      if (giveWayToSelector && selectorOpen) return false;
      // The status can be left over from an earlier try; a new event makes it this one's.
      if (WiFi.status() == WL_CONNECT_FAILED && disconnectEvents != eventsBefore) break;
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) break;
    taskLog("attempt %d failed, status %d, reason %d\n", attempt + 1, (int)WiFi.status(), (int)lastDisconnectReason);
    delay(1000);
  }
  const bool ok = WiFi.status() == WL_CONNECTED;
  taskLog("connect %s\n", ok ? "ok" : "failed");
  return ok;
}

// Makes it the network to return to, and saves that.
void adoptNetwork(const char *ssid, const char *pass) {
  const bool changed = savedSsid != ssid || savedPass != pass;
  savedSsid = ssid;
  savedPass = pass;
  known.remember(ssid, pass);
  if (changed) {
    Preferences net;
    net.begin("kmoni");
    net.putString("ssid", savedSsid);
    net.putString("pass", savedPass);
    net.putBytes("known", known.data(), KnownNetworks::byteSize());
    net.end();
  }
  Locked lock;
  strlcpy(wifiX.currentSsid, savedSsid.c_str(), sizeof wifiX.currentSsid);
}

void resumeSavedNetwork() {
  WiFi.disconnect();
  delay(200);
  WiFi.begin(savedSsid.c_str(), savedPass.isEmpty() ? nullptr : savedPass.c_str());
}

// When the last network isn't working: looks for any saved one in range, the last network
// first, then strongest first.
void joinKnownNetwork() {
  taskLog("Wi-Fi down, looking for saved networks\n");
  http.end();
  tcp.stop();
  WiFi.disconnect();
  delay(200);
  const int16_t found = WiFi.scanNetworks(false);

  KnownNetwork candidates[KnownNetworks::kMax];
  int count = 0;
  const KnownNetwork *last = known.find(savedSsid.c_str());
  for (int i = 0; i < found && count < KnownNetworks::kMax; i++) {
    const String ssid = WiFi.SSID(i);
    const KnownNetwork *k = known.find(ssid.c_str());
    if (!k) continue;
    bool duplicate = false;
    for (int j = 0; j < count; j++) duplicate = duplicate || ssid == candidates[j].ssid;
    if (duplicate) continue;
    if (k == last && count > 0) {
      memmove(&candidates[1], &candidates[0], sizeof(KnownNetwork) * count);
      candidates[0] = *k;
    } else {
      candidates[count] = *k;
    }
    count++;
  }
  WiFi.scanDelete();
  taskLog("%d saved networks in range\n", count);

  bool joined = false;
  for (int i = 0; i < count && !joined && !selectorOpen; i++) {
    joined = tryNetwork(candidates[i].ssid, candidates[i].pass, true, kSavedNetworkAttempts);
    if (joined) adoptNetwork(candidates[i].ssid, candidates[i].pass);
  }
  memset(candidates, 0, sizeof candidates);
  if (!joined && !selectorOpen) resumeSavedNetwork();
}

// Tries a network, keeping the old one until the new one has actually connected.
void connectTo(const char *ssid, const char *typedPass) {
  const KnownNetwork *saved = known.find(ssid);
  char pass[WifiSelector::kMaxPassword + 1];
  strlcpy(pass, typedPass[0] || !saved ? typedPass : saved->pass, sizeof pass);
  WiFi.scanDelete();
  scanRunning = false;
  const bool ok = tryNetwork(ssid, pass, false, typedPass[0] ? 1 : kSavedNetworkAttempts);
  if (ok) adoptNetwork(ssid, pass);
  else resumeSavedNetwork();
  wifiDownSinceMs = 0;
  memset(pass, 0, sizeof pass);
  Locked lock;
  wifiX.connectOk = ok;
  wifiX.connectDone = true;
}

void serviceWifiRequests() {
  bool scan, connect;
  char ssid[33] = {};
  char pass[WifiSelector::kMaxPassword + 1] = {};
  {
    Locked lock;
    scan = wifiX.scanRequested;
    connect = wifiX.connectRequested;
    wifiX.scanRequested = wifiX.connectRequested = false;
    if (connect) {
      strcpy(ssid, wifiX.connectSsid);
      strcpy(pass, wifiX.connectPass);
      memset(wifiX.connectPass, 0, sizeof wifiX.connectPass);
    }
  }
  if (connect) {
    connectTo(ssid, pass);
    memset(pass, 0, sizeof pass);
  }
  if (scan) beginScan();
  if (scanRunning) pollScan();
}

void applyRequests() {
  Locked lock;
  if (!shared.replayRequested) return;
  shared.replayRequested = false;
  replayFrom = shared.replayTarget;
  replayStartMs = millis();
  shared.replaying = replayFrom != 0;
  shownTime = 0;
  lastWarningId = "";
  lastForecastId = "";
}

void netTask(void *) {
  bool staticsLoaded = false;
  for (;;) {
    applyRequests();
    serviceWifiRequests();
    keepWifiAlive();
    const uint32_t now = millis();

    if (WiFi.status() == WL_CONNECTED) {
      if (!staticsLoaded) {
        taskLog("Wi-Fi up %s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        staticsLoaded = loadStaticLayers();
        taskLog("static layers %s\n", staticsLoaded ? "ok" : "failed");
      }
      if ((!wavWarning.data || !wavForecast.data) && (lastAlarmLoadMs == 0 || now - lastAlarmLoadMs > kAlarmRetryMs)) {
        loadAlarmSounds();
      }
      if (staticsLoaded && !replayFrom &&
          (syncedTime == 0 || now - syncedAtMs > kClockResyncMs) && now - lastSyncAttemptMs > 1000) {
        syncClock();
      }
      if (staticsLoaded && (syncedTime != 0 || replayFrom)) {
        const time_t want = wantedTime();
        if (want > shownTime) {
          const uint32_t t0 = millis();
          bool ok = buildFrame(want);
          time_t got = want;
          // Our second can tick just before the server publishes it; use the previous one.
          if (!ok && want - 1 > shownTime) {
            got = want - 1;
            ok = buildFrame(got);
            if (ok) framesFellBack++;
          }
          const uint32_t t1 = millis();
          if (ok) {
            publishFrame(got);
            shownTime = got;
            if (++framesShown % 30 == 1) {
              taskLog("frame %s fetch=%ums build=%ums rssi=%d heap=%u\n", stamp(got).c_str(),
                            (unsigned)(t1 - t0), (unsigned)(millis() - t1), WiFi.RSSI(),
                            (unsigned)ESP.getFreeHeap());
            }
          } else {
            framesFailed++;
          }
        }
      }
    }
    delay(20);
  }
}

}  // namespace

// TLS needs about 40 KB per connection, far more than the internal RAM can spare comfortably.
// Larger blocks go to PSRAM; small ones stay internal, for speed.
static void *tlsCalloc(size_t n, size_t size) {
  void *p = n * size >= 512 ? heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
}
static void tlsFree(void *p) { heap_caps_free(p); }

void setup() {
  mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
  auto cfg = M5.config();
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  M5.begin(cfg);

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block when USB isn't attached
  setenv("TZ", "UTC0", 1);   // see syncClock()
  tzset();

  prefs.begin("kmoni");
  menu.begin(prefs.getInt("bright", 80), prefs.getInt("sound", 50), prefs.getInt("autoret", 10));
  nightMode.mode = prefs.getInt("nightmode", nightMode.mode);
  nightMode.start = prefs.getInt("nightstart", nightMode.start);
  nightMode.end = prefs.getInt("nightend", nightMode.end);
  nightMode.wakeSeconds = prefs.getInt("nightwake", nightMode.wakeSeconds);
  nightMode.normalize();
  dimmer.setHoldMs((uint32_t)nightMode.wakeSeconds * 1000);
  homeSet = prefs.isKey("homelat");
  if (homeSet) {
    homeLat = prefs.getFloat("homelat");
    homeLon = prefs.getFloat("homelon");
  }
#ifdef HOME_LAT
  else if (!prefs.isKey("homecleared")) {
    homeSet = true;
    homeLat = HOME_LAT;
    homeLon = HOME_LON;
  }
#endif
  wifiFromNvs = prefs.isKey("ssid");
  savedSsid = wifiFromNvs ? prefs.getString("ssid") : String(WIFI_SSID);
  savedPass = wifiFromNvs ? prefs.getString("pass") : String(WIFI_PASS);
  if (prefs.getBytesLength("known") == KnownNetworks::byteSize()) {
    prefs.getBytes("known", known.data(), KnownNetworks::byteSize());
    known.normalize();
  }
  const int knownBefore = known.count();
  known.seed(savedSsid.c_str(), savedPass.c_str());
  known.seed(WIFI_SSID, WIFI_PASS);
  if (known.count() != knownBefore) prefs.putBytes("known", known.data(), KnownNetworks::byteSize());

  M5.Display.setRotation(0);  // the panel is natively 720x1280 portrait
  // A restart of our own stays dark at night. Being switched on or reset by hand shows the screen.
  const esp_reset_reason_t reset = esp_reset_reason();
  const bool ownRestart = reset == ESP_RST_SW || reset == ESP_RST_PANIC || reset == ESP_RST_INT_WDT ||
                          reset == ESP_RST_TASK_WDT || reset == ESP_RST_WDT;
  const bool startDark = ownRestart && nightMode.mode != NightSchedule::kOff;
  M5.Display.setBrightness(startDark ? 0 : (uint8_t)NightDimmer::levelOf(menu.brightness()));
  applySound(menu.sound());
  M5.Power.setBatteryCharge(true);  // a reset may have hit a battery pause
  Serial.printf("board=%d display=%dx%d psram=%u\n", (int)M5.getBoard(), M5.Display.width(),
                M5.Display.height(), (unsigned)ESP.getPsramSize());

  sharedLock = xSemaphoreCreateMutex();
  if (ownRestart && time(nullptr) > kClockPlausibleFrom) {
    shared.clockTime = time(nullptr);
    shared.clockAtMs = millis();
  }
  strlcpy(wifiX.currentSsid, savedSsid.c_str(), sizeof wifiX.currentSsid);
  download = psramAlloc<uint8_t>(kMaxDownload);
  gifWork = psramAlloc<uint8_t>(kSrcW * kSrcH);
  baseLayer = psramAlloc<uint32_t>(kSrcW * kSrcH);
  composed = psramAlloc<uint32_t>(kSrcW * kSrcH);
  upscaled = psramAlloc<uint32_t>(kMapW * kMapH);
  scaleLayer = psramAlloc<uint32_t>(kScaleW * kScaleH);
  layerBack = psramAlloc<uint16_t>((size_t)kScreenW * kLayerH);
  shared.layerFront = psramAlloc<uint16_t>((size_t)kScreenW * kLayerH);
  memset(shared.layerFront, 0xFF, (size_t)kScreenW * kLayerH * 2);
  resampleAxis(mapAxisX, kSrcW, kMapW);
  resampleAxis(mapAxisY, kSrcH, kMapH);

  quakeDownload = psramAlloc<uint8_t>(kQuakeDownloadCap);
  quakeRows = psramAlloc<QuakeSummary>(kQuakeRows);
  quakeX.list = psramAlloc<QuakeSummary>(kQuakeRows);
  quakeX.olderBatch = psramAlloc<QuakeSummary>(kOlderBatch);
  olderRows = psramAlloc<QuakeSummary>(kOlderRows);
  for (int i = 0; i < kQuakeSnapshotCount; i++) {
    quakeFromSnapshot(kQuakeSnapshot[i], kQuakeSnapshotNames[kQuakeSnapshot[i].name], &olderRows[i]);
  }
  olderCount = kQuakeSnapshotCount;
  history.setPreloaded(kQuakeSnapshotThrough);
  quakeShown = newQuakeDetail();
  quakeX.detail = newQuakeDetail();
  quakeMapShown = psramAlloc<uint16_t>((size_t)kQuakeMapW * kQuakeMapH);
  quakeX.map = psramAlloc<uint16_t>((size_t)kQuakeMapW * kQuakeMapH);
  browser.setRowJsonLookup(quakeRowJson);
  browser.setRowRankLookup(quakeRowRank);
  browser.setRowPartialLookup(quakeRowPartial);
  browser.setRowStorage(psramAlloc<int32_t>(QuakeBrowser::kMaxRows + 2));
  browser.olderLoaded(true, quakeRowTotal());  // there's a list to show before the first download

  canvas.setPsram(true);
  canvas.setColorDepth(16);
  panel.setPsram(true);
  panel.setColorDepth(16);
  modal.setPsram(true);
  modal.setColorDepth(16);
  if (!canvas.createSprite(kScreenW, kScreenH) || !panel.createSprite(SettingsMenu::kPanelW, SettingsMenu::kPanelH) ||
      !modal.createSprite(kScreenW, kScreenH)) {
    Serial.println("canvas alloc failed, rebooting");
    delay(2000);
    ESP.restart();
  }
  canvas.fillScreen(kPaper);
  drawPanel();
  bootMs = millis();
  render();
  if (nightMode.mode != NightSchedule::kOff && !ownRestart) dimmer.wake(millis());

  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    lastDisconnectReason = info.wifi_sta_disconnected.reason;
    disconnectEvents = disconnectEvents + 1;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.persistent(false);  // only networks that worked get saved, by connectTo()
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(savedSsid.c_str(), savedPass.isEmpty() ? nullptr : savedPass.c_str());
  xTaskCreatePinnedToCore(netTask, "net", 16 * 1024, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(quakeTask, "quake", 20 * 1024, nullptr, 1, nullptr, 0);
}

void loop() {
  const uint32_t started = millis();
  M5.update();
  handleSerial();
  updateBattery();

  bool pressed;
  int x, y;
  if (injectActive) {
    pressed = injectPressed;
    x = injectX;
    y = injectY;
    if (!injectPressed) injectActive = false;
  } else {
    const auto &d = M5.Touch.getDetail();
    pressed = M5.Touch.getCount() > 0 && d.isPressed();
    x = d.x;
    y = d.y;
    static bool wasDown = false;
    if (pressed && !wasDown) touchesSeen++;
    wasDown = pressed;
  }
  updateBrightness(pressed);
  if (dimmer.wakeTouch()) pressed = false;

  static bool renderForced = false;
  // A slide between the live page and the list follows the finger until it comes to rest.
  if (pageSlide.active()) {
    pageSlide.update(pressed, x, millis());
    pumpQuakeExchange();
    pumpWifiExchange();
    playPendingAlarm();
    if (!browser.visible()) pageSlide.cancel();  // an alert came in
    if (browser.visible()) {
      static int shownAt = -1;
      const bool changed = browser.takeDirty();
      if (changed) composeQuakePage();
      if (changed || pageSlide.position() != shownAt) {
        shownAt = pageSlide.position();
        showPageSlide(shownAt);
      }
    }
    if (pageSlide.takeFinished() && pageSlide.listShown()) {
      browser.resume(millis());
    } else if (!pageSlide.active() && (!browser.visible() || !pageSlide.listShown())) {
      browser.close();
      browser.takeDirty();
      watchdogGraceFromMs = millis();
      renderForced = true;
    }
    uint32_t took = millis() - started;
    if (took > uiWorstMs) uiWorstMs = took;
    delay(2);
    return;
  }
  // The earthquake pages take over the whole screen and all input while they're up.
  if (browser.visible()) {
    browser.update(pressed, x, y, millis());
    pumpQuakeExchange();
    // Keeps "34 minutes ago" true while the page is left open.
    static uint32_t agoMinute = 0;
    if (millis() / 60000 != agoMinute) {
      agoMinute = millis() / 60000;
      browser.redraw();
    }
    pumpWifiExchange();
    playPendingAlarm();
    if (browser.visible()) {
      int fingerX;
      if (browser.takeBackDrag(&fingerX)) {
        composeCanvas();  // it has been idle while the list was up
        pageSlide.begin(PageSlide::kFull, fingerX, millis());
      } else if (browser.takeDirty()) {
        drawQuakePage();
      }
    } else {
      watchdogGraceFromMs = millis();
      renderForced = true;
    }
    uint32_t took = millis() - started;
    if (took > uiWorstMs) uiWorstMs = took;
    delay(5);
    return;
  }
  pumpQuakeExchange();

  // Night mode, the location screen and the selector take over the same way.
  if (nightEditor.visible()) {
    nightEditor.update(pressed, x, y, millis());
    playPendingAlarm();
    if (nightEditor.takeSaved(&nightMode)) {
      prefs.putInt("nightmode", nightMode.mode);
      prefs.putInt("nightstart", nightMode.start);
      prefs.putInt("nightend", nightMode.end);
      prefs.putInt("nightwake", nightMode.wakeSeconds);
      dimmer.setHoldMs((uint32_t)nightMode.wakeSeconds * 1000);
      drawPanel();
    }
    if (nightEditor.visible()) {
      if (nightEditor.takeDirty()) drawNightScreen();
    } else {
      nightEditor.takeDirty();
      watchdogGraceFromMs = millis();
      renderForced = true;
    }
    uint32_t took = millis() - started;
    if (took > uiWorstMs) uiWorstMs = took;
    delay(5);
    return;
  }
  if (locationEditor.visible()) {
    locationEditor.update(pressed, x, y, millis());
    playPendingAlarm();
    float lat, lon;
    if (locationEditor.takeSaved(&lat, &lon)) {
      homeLat = lat;
      homeLon = lon;
      homeSet = true;
      prefs.putFloat("homelat", lat);
      prefs.putFloat("homelon", lon);
      prefs.remove("homecleared");
      drawPanel();
    }
    if (locationEditor.takeCleared()) {
      homeSet = false;
      prefs.remove("homelat");
      prefs.remove("homelon");
      prefs.putBool("homecleared", true);  // so the secrets.h default doesn't come back
      drawPanel();
    }
    if (locationEditor.visible()) {
      if (locationEditor.takeDirty()) drawLocationScreen();
    } else {
      locationEditor.takeDirty();
      watchdogGraceFromMs = millis();
      renderForced = true;
    }
    uint32_t took = millis() - started;
    if (took > uiWorstMs) uiWorstMs = took;
    delay(5);
    return;
  }
  if (selector.visible()) {
    selector.update(pressed, x, y, millis());
    pumpWifiExchange();
    playPendingAlarm();
    if (selector.visible()) {
      if (selector.takeDirty()) drawSelector();
    } else {
      selectorOpen = false;
      watchdogGraceFromMs = millis();
      renderForced = true;
    }
    uint32_t took = millis() - started;
    if (took > uiWorstMs) uiWorstMs = took;
    delay(5);
    return;
  }
  pumpWifiExchange();

  menu.update(pressed, x, y, millis());
  if (menu.takeSoundChanged()) {
    applySound(menu.sound());
    // Let the new level be heard; 0 also silences anything playing.
    if (menu.sound() > 0) M5.Speaker.tone(1000, 120);
    else M5.Speaker.stop();
  }
  if (menu.takeSaveRequest()) {
    prefs.putInt("bright", menu.brightness());
    prefs.putInt("sound", menu.sound());
    prefs.putInt("autoret", menu.autoReturnSeconds());
  }
  // Fed every sample, so a swipe is judged from where it really started.
  if (edgeSwipe.update(pressed, x, y) && menu.position() == 0 && !menu.moving()) {
    browser.setAutoReturnMs((uint32_t)menu.autoReturnSeconds() * 1000);
    browser.open(millis());
    pumpQuakeExchange();
    browser.takeDirty();
    composeCanvas();
    composeQuakePage();
    pageSlide.begin(0, edgeSwipe.startX(), millis());
    edgeSwipe.reset();
    delay(2);
    return;
  }
  if (menu.takeLocationOpenRequest()) {
    menu.closeNow();
    panelShownPos = 0;
    locationEditor.open(homeSet, homeLat, homeLon, millis());
    locationEditor.takeDirty();
    drawLocationScreen();
    delay(5);
    return;
  }
  if (menu.takeNightOpenRequest()) {
    menu.closeNow();
    panelShownPos = 0;
    nightEditor.open(nightMode, millis());
    nightEditor.takeDirty();
    drawNightScreen();
    delay(5);
    return;
  }
  if (menu.takeWifiOpenRequest()) {
    menu.closeNow();
    panelShownPos = 0;
    selector.open(millis());
    pumpWifiExchange();
    selector.takeDirty();
    drawSelector();
    delay(5);
    return;
  }
  bool looksChanged = menu.takeLooksChanged();
  // The Wi-Fi status may have changed since the panel was last shown.
  if (panelShownPos == 0 && menu.position() > 0) looksChanged = true;
  if (looksChanged) drawPanel();
  if (looksChanged || menu.position() != panelShownPos) showPanelAt(menu.position());

  playPendingAlarm();

  // Frames wait while the panel is moving, so the finger never lags behind a redraw.
  static uint32_t renderedSeq = 0, lastRenderMs = 0;
  if (!menu.moving()) {
    uint32_t seq;
    {
      Locked lock;
      seq = shared.frameSeq;
    }
    // Between frames, one refresh a second keeps the status current.
    if (renderForced || seq != renderedSeq || millis() - lastRenderMs > 1000) {
      renderForced = false;
      renderedSeq = seq;
      render();
      lastRenderMs = millis();
    }
  }

  const uint32_t took = millis() - started;
  if (took > uiWorstMs) uiWorstMs = took;
  delay(5);
}
