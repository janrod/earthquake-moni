// Parses JMA's earthquake list and report JSON (www.jma.go.jp/bosai/quake/data).
// Host-testable: only needs ArduinoJson. Output strings are UTF-8.
#pragma once
#include <ArduinoJson.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Intensity as a rank: 0 none, 1-4, 5 = 5-, 6 = 5+, 7 = 6-, 8 = 6+, 9 = 7.
// JMA can prefix "!" for "5- or above, not yet received", which ranks the same.
inline int quakeIntensityRank(const char *s) {
  if (!s) return 0;
  if (*s == '!') s++;
  if (s[0] < '1' || s[0] > '7') return 0;
  const bool plus = s[1] == '+', minus = s[1] == '-';
  switch (s[0]) {
    case '5': return plus ? 6 : minus ? 5 : 5;
    case '6': return plus ? 8 : minus ? 7 : 7;
    case '7': return 9;
    default: return s[0] - '0';
  }
}

inline const char *quakeIntensityLabel(int rank) {
  static const char *const labels[] = {"-", "1", "2", "3", "4", "5弱", "5強", "6弱", "6強", "7"};
  return labels[rank >= 0 && rank <= 9 ? rank : 0];
}

// Before 1996 the scale had a plain 5 and 6, with no 弱/強. Those rank as the lower half for
// filtering, but show as recorded.
inline const char *quakeIntensityLabel(int rank, bool legacyScale) {
  if (legacyScale && rank == 5) return "5";
  if (legacyScale && rank == 7) return "6";
  return quakeIntensityLabel(rank);
}

// The database writes intensities in Japanese: "震度５弱". Sets *legacy for a plain 5 or 6.
inline int quakeRankFromJapanese(const char *s, bool *legacy = nullptr) {
  if (legacy) *legacy = false;
  if (!s) return 0;
  static const struct { const char *text; int rank; bool legacy; } table[] = {
      {"震度５弱", 5, false}, {"震度５強", 6, false}, {"震度６弱", 7, false}, {"震度６強", 8, false},
      {"震度１", 1, false},   {"震度２", 2, false},   {"震度３", 3, false},   {"震度４", 4, false},
      {"震度７", 9, false},   {"震度５", 5, true},    {"震度６", 7, true},
  };
  for (const auto &t : table) {
    if (strcmp(s, t.text) == 0) {
      if (legacy) *legacy = t.legacy;
      return t.rank;
    }
  }
  return 0;
}

// Great-circle distance. Enough for this: the epicentres themselves are only given to 0.1 degree.
inline float quakeDistanceKm(float lat1, float lon1, float lat2, float lon2) {
  const double rad = 3.14159265358979 / 180;
  const double dLat = (lat2 - lat1) * rad, dLon = (lon2 - lon1) * rad;
  const double a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1 * rad) * cos(lat2 * rad) * sin(dLon / 2) * sin(dLon / 2);
  return (float)(6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a)));
}

// JMA's intensity colours, 0xRRGGBB.
inline uint32_t quakeIntensityColor(int rank) {
  static const uint32_t colors[] = {0xE0E0E0, 0xF2F2FF, 0x00AAFF, 0x0041FF, 0xFAE696,
                                    0xFFE600, 0xFF9900, 0xFF2800, 0xA50021, 0xB40068};
  return colors[rank >= 0 && rank <= 9 ? rank : 0];
}

// Dark text on the pale colours, white on the strong ones.
inline uint32_t quakeIntensityInk(int rank) {
  return rank == 3 || rank >= 7 ? 0xFFFFFF : 0x222222;
}

// "+35.7+140.6-50000/" = latitude, longitude, depth in metres below sea level (optional).
inline bool quakeParseCoordinate(const char *s, float *lat, float *lon, int *depthKm) {
  *depthKm = -1;
  if (!s) return false;
  float la, lo;
  int metres;
  const int n = sscanf(s, "%f%f%d", &la, &lo, &metres);
  if (n < 2) return false;
  *lat = la;
  *lon = lo;
  if (n == 3) *depthKm = (metres < 0 ? -metres : metres) / 1000;
  return true;
}

enum QuakeTimePrecision { kQuakeTimeExact, kQuakeTimeHour, kQuakeTimeMonth };

struct QuakeSummary {
  char eventId[16];
  char json[64];
  char name[64];
  char time[20];  // "2026-11-03T14:40:00"
  char magnitude[8];
  int8_t maxRank;
  int8_t timePrecision;
  int16_t depthKm;  // -1 unknown
  float lat, lon;
  bool hasCoordinate;
  bool jsonHasStations;
  bool legacyScale;
};

namespace quake_detail {
inline void copy(char *dst, size_t cap, const char *src) {
  if (src) snprintf(dst, cap, "%s", src);
}
}  // namespace quake_detail

// One row per earthquake, newest first. The list holds several reports per event, so each
// row combines them: newest wins, older ones fill the gaps. Events whose newest report is a
// cancellation are dropped, as are notices that aren't about one earthquake.
// Returns the row count, or -1 if the data couldn't be parsed.
inline int quakeParseList(const char *data, size_t len, QuakeSummary *out, int maxRows,
                          ArduinoJson::Allocator *alloc = ArduinoJson::detail::DefaultAllocator::instance()) {
  JsonDocument filter;
  JsonObject item = filter.add<JsonObject>();
  for (const char *k : {"ttl", "eid", "ift", "at", "anm", "cod", "mag", "maxi", "json"}) item[k] = true;
  JsonDocument doc(alloc);
  if (deserializeJson(doc, data, len, DeserializationOption::Filter(filter))) return -1;
  JsonArrayConst list = doc.as<JsonArrayConst>();
  if (list.isNull()) return -1;

  int count = 0;
  static const int kMaxCancelled = 16;
  char cancelled[kMaxCancelled][16];
  int cancelledCount = 0;
  for (JsonObjectConst e : list) {
    const char *title = e["ttl"];
    const char *id = e["eid"];
    if (!title || !id || !id[0]) continue;
    const bool hasBoth = strcmp(title, "震源・震度情報") == 0;
    if (!hasBoth && strcmp(title, "震度速報") != 0 && strcmp(title, "震源に関する情報") != 0 &&
        strcmp(title, "遠地地震に関する情報") != 0)
      continue;

    bool skip = false;
    for (int i = 0; i < cancelledCount; i++) skip = skip || strcmp(cancelled[i], id) == 0;
    if (skip) continue;

    int row = -1;
    for (int i = 0; i < count; i++) {
      if (strcmp(out[i].eventId, id) == 0) row = i;
    }
    const char *infoType = e["ift"];
    if (row < 0 && infoType && strcmp(infoType, "取消") == 0) {
      if (cancelledCount < kMaxCancelled) snprintf(cancelled[cancelledCount++], 16, "%s", id);
      continue;
    }
    if (row < 0) {
      if (count == maxRows) continue;
      row = count++;
      memset(&out[row], 0, sizeof(QuakeSummary));
      out[row].depthKm = -1;
      quake_detail::copy(out[row].eventId, sizeof out[row].eventId, id);
    }
    QuakeSummary &q = out[row];
    if (!q.name[0]) quake_detail::copy(q.name, sizeof q.name, e["anm"]);
    if (!q.time[0]) {
      const char *at = e["at"];
      if (at && at[0]) snprintf(q.time, sizeof q.time, "%.19s", at);
    }
    if (!q.magnitude[0]) quake_detail::copy(q.magnitude, sizeof q.magnitude, e["mag"]);
    if (!q.maxRank) q.maxRank = (int8_t)quakeIntensityRank(e["maxi"]);
    if (!q.hasCoordinate) {
      float lat, lon;
      int depth;
      const char *cod = e["cod"];
      if (cod && quakeParseCoordinate(cod, &lat, &lon, &depth)) {
        q.lat = lat;
        q.lon = lon;
        q.depthKm = (int16_t)depth;
        q.hasCoordinate = true;
      }
    }
    // The report with both hypocentre and stations is the one worth opening.
    if (!q.json[0] || (hasBoth && !q.jsonHasStations)) {
      quake_detail::copy(q.json, sizeof q.json, e["json"]);
      q.jsonHasStations = hasBoth;
    }
  }
  // JMA orders by report time, so a late report can put an older earthquake above a newer one.
  for (int i = 1; i < count; i++) {
    const QuakeSummary cur = out[i];
    int j = i - 1;
    while (j >= 0 && strcmp(out[j].time, cur.time) < 0) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = cur;
  }
  return count;
}

struct QuakeStation {
  float lat, lon;
  int8_t rank;
};

struct QuakeArea {
  int code;
  int8_t rank;
  char name[64];
};

struct QuakeDetail {
  char name[64];
  char time[20];
  char magnitude[8];
  char comment[200];
  int8_t maxRank;
  int16_t depthKm;
  float lat, lon;
  bool hasCoordinate;
  bool legacyScale;
  int8_t timePrecision;
  bool areasAreStations;  // the database has no region names, so `areas` holds the strongest stations
  QuakeStation *stations;
  int stationCount, stationCap;
  QuakeArea *areas;
  int areaCount, areaCap;
};

// Fills `d`, keeping its arrays. Areas come out strongest first.
inline bool quakeParseDetail(const char *data, size_t len, QuakeDetail *d,
                             ArduinoJson::Allocator *alloc = ArduinoJson::detail::DefaultAllocator::instance()) {
  JsonDocument filter;
  filter["Body"]["Earthquake"] = true;
  filter["Body"]["Comments"]["ForecastComment"]["Text"] = true;
  JsonObject obs = filter["Body"]["Intensity"]["Observation"].to<JsonObject>();
  obs["MaxInt"] = true;
  JsonObject area = obs["Pref"].add<JsonObject>()["Area"].add<JsonObject>();
  area["Name"] = true;
  area["Code"] = true;
  area["MaxInt"] = true;
  JsonObject st = area["City"].add<JsonObject>()["IntensityStation"].add<JsonObject>();
  st["Int"] = true;
  st["latlon"] = true;

  JsonDocument doc(alloc);
  if (deserializeJson(doc, data, len, DeserializationOption::Filter(filter),
                      DeserializationOption::NestingLimit(24)))
    return false;
  JsonObjectConst body = doc["Body"];
  if (body.isNull()) return false;

  d->name[0] = d->time[0] = d->magnitude[0] = d->comment[0] = 0;
  d->maxRank = 0;
  d->depthKm = -1;
  d->hasCoordinate = false;
  d->legacyScale = false;
  d->timePrecision = kQuakeTimeExact;
  d->areasAreStations = false;
  d->stationCount = d->areaCount = 0;

  JsonObjectConst eq = body["Earthquake"];
  if (!eq.isNull()) {
    const char *at = eq["OriginTime"];
    if (at) snprintf(d->time, sizeof d->time, "%.19s", at);
    quake_detail::copy(d->magnitude, sizeof d->magnitude, eq["Magnitude"]);
    quake_detail::copy(d->name, sizeof d->name, eq["Hypocenter"]["Area"]["Name"]);
    float lat, lon;
    int depth;
    const char *cod = eq["Hypocenter"]["Area"]["Coordinate"];
    if (cod && quakeParseCoordinate(cod, &lat, &lon, &depth)) {
      d->lat = lat;
      d->lon = lon;
      d->depthKm = (int16_t)depth;
      d->hasCoordinate = true;
    }
  }
  quake_detail::copy(d->comment, sizeof d->comment, body["Comments"]["ForecastComment"]["Text"]);

  JsonObjectConst observation = body["Intensity"]["Observation"];
  d->maxRank = (int8_t)quakeIntensityRank(observation["MaxInt"]);
  for (JsonObjectConst pref : observation["Pref"].as<JsonArrayConst>()) {
    for (JsonObjectConst a : pref["Area"].as<JsonArrayConst>()) {
      if (d->areaCount < d->areaCap) {
        QuakeArea &out = d->areas[d->areaCount++];
        const char *code = a["Code"];
        out.code = code ? atoi(code) : 0;
        out.rank = (int8_t)quakeIntensityRank(a["MaxInt"]);
        out.name[0] = 0;
        quake_detail::copy(out.name, sizeof out.name, a["Name"]);
      }
      for (JsonObjectConst city : a["City"].as<JsonArrayConst>()) {
        for (JsonObjectConst s : city["IntensityStation"].as<JsonArrayConst>()) {
          JsonObjectConst ll = s["latlon"];
          if (ll.isNull() || d->stationCount >= d->stationCap) continue;
          QuakeStation &out = d->stations[d->stationCount++];
          out.lat = ll["lat"];
          out.lon = ll["lon"];
          out.rank = (int8_t)quakeIntensityRank(s["Int"]);
        }
      }
    }
  }
  // Strongest first (stable, so JMA's order is kept within a level).
  for (int i = 1; i < d->areaCount; i++) {
    const QuakeArea cur = d->areas[i];
    int j = i - 1;
    while (j >= 0 && d->areas[j].rank < cur.rank) {
      d->areas[j + 1] = d->areas[j];
      j--;
    }
    d->areas[j + 1] = cur;
  }
  return true;
}

namespace quake_detail {
// "2011/03/11 14:46:18.1" -> "2011-03-11T14:46:18"
// Always the sortable form. Older records can be vaguer: "2000/08/04 04:24", "1935/07/18 03時",
// "1962年08月". Returns what's known, as a QuakeTimePrecision.
inline int dbTime(char *out, size_t cap, const char *ot) {
  if (!ot) ot = "";
  const size_t n = strlen(ot);
  int precision = kQuakeTimeExact;
  if (n > 4 && (unsigned char)ot[4] > 0x7F) {
    snprintf(out, cap, "%.4s-%.2s-01T00:00:00", ot, n > 7 ? ot + 7 : "01");
    return kQuakeTimeMonth;
  }
  if (n > 13 && (unsigned char)ot[13] > 0x7F) {
    snprintf(out, cap, "%.13s:00:00", ot);
    precision = kQuakeTimeHour;
  } else if (n == 16) {
    snprintf(out, cap, "%s:00", ot);
  } else {
    snprintf(out, cap, "%.19s", ot);
  }
  for (char *c = out; *c; c++) {
    if (*c == '/') *c = '-';
    if (*c == ' ') *c = 'T';
  }
  return precision;
}
// The database writes "0.0000" for an epicentre it doesn't know.
inline bool dbHasCoordinate(const char *lat, const char *lon) { return lat && lon && lat[0] && lon[0] && (atof(lat) != 0 || atof(lon) != 0); }
// "24 km" -> 24; "不明" (unknown) -> -1
inline int dbDepth(const char *s) { return s && s[0] >= '0' && s[0] <= '9' ? atoi(s) : -1; }
inline void dbMagnitude(char *out, size_t cap, const char *s) {
  out[0] = 0;
  if (s && s[0] >= '0' && s[0] <= '9') snprintf(out, cap, "%s", s);
}
}  // namespace quake_detail

// JMA's long-term intensity database: a search, newest first. Returns the row count, or -1 if
// it answered with a message rather than rows. Its reports open with "db:" and the event id.
inline int quakeParseDbSearch(const char *data, size_t len, QuakeSummary *out, int maxRows,
                              ArduinoJson::Allocator *alloc = ArduinoJson::detail::DefaultAllocator::instance()) {
  JsonDocument doc(alloc);
  if (deserializeJson(doc, data, len)) return -1;
  JsonVariantConst res = doc["res"];
  if (!res.is<JsonArrayConst>()) return -1;
  int n = 0;
  for (JsonObjectConst e : res.as<JsonArrayConst>()) {
    if (n == maxRows) break;
    QuakeSummary &q = out[n];
    memset(&q, 0, sizeof q);
    const char *id = e["id"];
    if (!id) continue;
    quake_detail::copy(q.eventId, sizeof q.eventId, id);
    snprintf(q.json, sizeof q.json, "db:%s", id);
    quake_detail::copy(q.name, sizeof q.name, e["name"]);
    q.timePrecision = (int8_t)quake_detail::dbTime(q.time, sizeof q.time, e["ot"]);
    quake_detail::dbMagnitude(q.magnitude, sizeof q.magnitude, e["mag"]);
    q.depthKm = (int16_t)quake_detail::dbDepth(e["dep"]);
    bool legacy = false;
    q.maxRank = (int8_t)quakeRankFromJapanese(e["maxI"], &legacy);
    q.legacyScale = legacy;
    const char *lat = e["lat"];
    const char *lon = e["lon"];
    q.hasCoordinate = quake_detail::dbHasCoordinate(lat, lon);
    if (q.hasCoordinate) {
      q.lat = (float)atof(lat);
      q.lon = (float)atof(lon);
    }
    q.jsonHasStations = true;
    n++;
  }
  // Newest first, by the times as they now read: vague ones arrive out of that order.
  qsort(out, n, sizeof *out, [](const void *pa, const void *pb) {
    const QuakeSummary *a = static_cast<const QuakeSummary *>(pa), *b = static_cast<const QuakeSummary *>(pb);
    const int t = strcmp(b->time, a->time);
    return t ? t : strcmp(b->eventId, a->eventId);
  });
  return n;
}

// A database report: the epicentre and every station.
inline bool quakeParseDbEvent(const char *data, size_t len, QuakeDetail *d,
                              ArduinoJson::Allocator *alloc = ArduinoJson::detail::DefaultAllocator::instance()) {
  JsonDocument filter;
  filter["res"]["hyp"] = true;
  JsonObject st = filter["res"]["int"].add<JsonObject>();
  for (const char *k : {"name", "lat", "lon", "int"}) st[k] = true;
  JsonDocument doc(alloc);
  if (deserializeJson(doc, data, len, DeserializationOption::Filter(filter))) return false;
  JsonObjectConst hyp = doc["res"]["hyp"][0];
  if (hyp.isNull()) return false;

  d->name[0] = d->time[0] = d->magnitude[0] = d->comment[0] = 0;
  d->stationCount = d->areaCount = 0;
  d->areasAreStations = true;
  quake_detail::copy(d->name, sizeof d->name, hyp["name"]);
  d->timePrecision = (int8_t)quake_detail::dbTime(d->time, sizeof d->time, hyp["ot"]);
  quake_detail::dbMagnitude(d->magnitude, sizeof d->magnitude, hyp["mag"]);
  d->depthKm = (int16_t)quake_detail::dbDepth(hyp["dep"]);
  bool legacy = false;
  d->maxRank = (int8_t)quakeRankFromJapanese(hyp["maxI"], &legacy);
  d->legacyScale = legacy;
  const char *lat = hyp["lat"];
  const char *lon = hyp["lon"];
  d->hasCoordinate = quake_detail::dbHasCoordinate(lat, lon);
  if (d->hasCoordinate) {
    d->lat = (float)atof(lat);
    d->lon = (float)atof(lon);
  }
  // JMA lists these strongest first, so the first few are the ones worth naming.
  for (JsonObjectConst s : doc["res"]["int"].as<JsonArrayConst>()) {
    const int rank = quakeRankFromJapanese(s["int"]);
    const char *sLat = s["lat"];
    const char *sLon = s["lon"];
    if (sLat && sLon && d->stationCount < d->stationCap) {
      QuakeStation &out = d->stations[d->stationCount++];
      out.lat = (float)atof(sLat);
      out.lon = (float)atof(sLon);
      out.rank = (int8_t)rank;
    }
    if (d->areaCount < d->areaCap) {
      QuakeArea &a = d->areas[d->areaCount++];
      a.code = 0;
      a.rank = (int8_t)rank;
      a.name[0] = 0;
      quake_detail::copy(a.name, sizeof a.name, s["name"]);
    }
  }
  return true;
}
