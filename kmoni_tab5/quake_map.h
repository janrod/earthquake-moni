// Draws the affected area of an earthquake from JMA's region outlines, into a byte-swapped
// RGB565 buffer. Host-testable: only needs ArduinoJson.
#pragma once
#include <ArduinoJson.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "quake_data.h"

struct QuakeRegions {
  struct Region {
    int code;
    int firstRing, ringCount;
    float minLon, maxLon, minLat, maxLat;
  };
  struct Ring {
    int firstPoint, pointCount;
  };
  struct Point {
    float lon, lat;
  };
  Region *regions = nullptr;
  Ring *rings = nullptr;
  Point *points = nullptr;
  int regionCount = 0, ringCount = 0, pointCount = 0;
  bool loaded() const { return regionCount > 0; }
};

// Parses JMA's bosai/common/const/geojson/earthquake.json. `alloc` must not return null.
inline bool quakeLoadRegions(const char *data, size_t len, QuakeRegions *r, void *(*alloc)(size_t),
                             ArduinoJson::Allocator *jsonAlloc = ArduinoJson::detail::DefaultAllocator::instance()) {
  JsonDocument doc(jsonAlloc);
  if (deserializeJson(doc, data, len, DeserializationOption::NestingLimit(16))) return false;
  JsonArrayConst features = doc["features"];
  if (features.isNull()) return false;

  int regions = 0, rings = 0, points = 0;
  for (JsonObjectConst f : features) {
    regions++;
    for (JsonArrayConst polygon : f["geometry"]["coordinates"].as<JsonArrayConst>()) {
      for (JsonArrayConst ring : polygon) {
        rings++;
        points += ring.size();
      }
    }
  }
  if (regions == 0 || points == 0) return false;
  r->regions = static_cast<QuakeRegions::Region *>(alloc(sizeof(QuakeRegions::Region) * regions));
  r->rings = static_cast<QuakeRegions::Ring *>(alloc(sizeof(QuakeRegions::Ring) * rings));
  r->points = static_cast<QuakeRegions::Point *>(alloc(sizeof(QuakeRegions::Point) * points));

  int ri = 0, ringAt = 0, pointAt = 0;
  for (JsonObjectConst f : features) {
    QuakeRegions::Region &region = r->regions[ri++];
    const char *code = f["properties"]["code"];
    region.code = code ? atoi(code) : 0;
    region.firstRing = ringAt;
    region.minLon = region.minLat = 1e9f;
    region.maxLon = region.maxLat = -1e9f;
    for (JsonArrayConst polygon : f["geometry"]["coordinates"].as<JsonArrayConst>()) {
      for (JsonArrayConst ring : polygon) {
        QuakeRegions::Ring &outRing = r->rings[ringAt++];
        outRing.firstPoint = pointAt;
        outRing.pointCount = ring.size();
        for (JsonArrayConst p : ring) {
          const float lon = p[0], lat = p[1];
          r->points[pointAt++] = {lon, lat};
          if (lon < region.minLon) region.minLon = lon;
          if (lon > region.maxLon) region.maxLon = lon;
          if (lat < region.minLat) region.minLat = lat;
          if (lat > region.maxLat) region.maxLat = lat;
        }
      }
    }
    region.ringCount = ringAt - region.firstRing;
  }
  r->regionCount = regions;
  r->ringCount = rings;
  r->pointCount = points;
  return true;
}

struct QuakeMapView {
  float centerLon, centerLat, pxPerDegLat, pxPerDegLon;
  int w, h;
  float x(float lon) const { return w / 2.0f + (lon - centerLon) * pxPerDegLon; }
  float y(float lat) const { return h / 2.0f - (lat - centerLat) * pxPerDegLat; }
};

// Frames the stations and the epicentre with some margin. If there are no stations, the listed
// regions are used, and failing that the epicentre alone gets a fixed-size view.
inline QuakeMapView quakeFitView(const QuakeRegions &regions, const QuakeDetail &d, int w, int h) {
  static constexpr float kMinSpanLat = 1.2f;  // the outlines get coarse when zoomed further
  static constexpr float kMargin = 0.14f;
  float minLon = 1e9f, maxLon = -1e9f, minLat = 1e9f, maxLat = -1e9f;
  auto add = [&](float lon, float lat) {
    if (lon < minLon) minLon = lon;
    if (lon > maxLon) maxLon = lon;
    if (lat < minLat) minLat = lat;
    if (lat > maxLat) maxLat = lat;
  };
  for (int i = 0; i < d.stationCount; i++) add(d.stations[i].lon, d.stations[i].lat);
  if (d.stationCount == 0) {
    for (int a = 0; a < d.areaCount; a++) {
      for (int i = 0; i < regions.regionCount; i++) {
        const auto &rg = regions.regions[i];
        if (rg.code != d.areas[a].code) continue;
        add(rg.minLon, rg.minLat);
        add(rg.maxLon, rg.maxLat);
      }
    }
  }
  if (d.hasCoordinate) add(d.lon, d.lat);
  if (minLon > maxLon) {  // nothing to frame: all of Japan
    minLon = 128;
    maxLon = 146;
    minLat = 30;
    maxLat = 46;
  }
  QuakeMapView v;
  v.w = w;
  v.h = h;
  v.centerLon = (minLon + maxLon) / 2;
  v.centerLat = (minLat + maxLat) / 2;
  const float cosLat = cosf(v.centerLat * 3.14159265f / 180);
  float spanLat = (maxLat - minLat) * (1 + 2 * kMargin);
  float spanLon = (maxLon - minLon) * (1 + 2 * kMargin);
  if (spanLat < kMinSpanLat) spanLat = kMinSpanLat;
  const float byLat = h / spanLat;
  const float byLon = spanLon > 0 ? w / (spanLon * cosLat) : byLat;
  v.pxPerDegLat = byLat < byLon ? byLat : byLon;
  v.pxPerDegLon = v.pxPerDegLat * cosLat;
  return v;
}

namespace quake_draw {

inline uint16_t swap565(uint32_t rgb) {
  const uint16_t c = (uint16_t)(((rgb >> 16) & 0xF8) << 8 | ((rgb >> 8) & 0xFC) << 3 | (rgb & 0xFF) >> 3);
  return (uint16_t)(c << 8 | c >> 8);
}

inline uint32_t mixWithWhite(uint32_t rgb, int percentColor) {
  uint32_t out = 0;
  for (int shift = 16; shift >= 0; shift -= 8) {
    const int c = (rgb >> shift) & 0xFF;
    out |= (uint32_t)((c * percentColor + 255 * (100 - percentColor)) / 100) << shift;
  }
  return out;
}

inline void rect(uint16_t *fb, int w, int h, int x, int y, int rw, int rh, uint16_t c) {
  for (int py = y < 0 ? 0 : y; py < y + rh && py < h; py++) {
    uint16_t *row = fb + (size_t)py * w;
    for (int px = x < 0 ? 0 : x; px < x + rw && px < w; px++) row[px] = c;
  }
}

inline void line(uint16_t *fb, int w, int h, float x0, float y0, float x1, float y1, uint16_t c, int thickness = 1) {
  if ((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) || (x0 >= w && x1 >= w) || (y0 >= h && y1 >= h)) return;
  const float dx = x1 - x0, dy = y1 - y0;
  const float big = fabsf(dx) > fabsf(dy) ? fabsf(dx) : fabsf(dy);
  const int steps = big < 1 ? 1 : (int)big;
  for (int i = 0; i <= steps; i++) {
    const int px = (int)(x0 + dx * i / steps), py = (int)(y0 + dy * i / steps);
    if (thickness == 1) {
      if (px >= 0 && py >= 0 && px < w && py < h) fb[(size_t)py * w + px] = c;
    } else {
      rect(fb, w, h, px - thickness / 2, py - thickness / 2, thickness, thickness, c);
    }
  }
}

// Even-odd scanline fill of every ring of a region together, so holes come out right.
inline void fillRegion(const QuakeRegions &r, const QuakeRegions::Region &rg, const QuakeMapView &v, uint16_t *fb, uint16_t c) {
  static constexpr int kMaxCrossings = 512;
  float crossings[kMaxCrossings];
  int top = (int)floorf(v.y(rg.maxLat)), bottom = (int)ceilf(v.y(rg.minLat));
  if (top < 0) top = 0;
  if (bottom >= v.h) bottom = v.h - 1;
  for (int py = top; py <= bottom; py++) {
    const float sampleLat = v.centerLat - (py + 0.5f - v.h / 2.0f) / v.pxPerDegLat;
    int n = 0;
    for (int k = 0; k < rg.ringCount; k++) {
      const auto &ring = r.rings[rg.firstRing + k];
      const QuakeRegions::Point *pts = r.points + ring.firstPoint;
      for (int i = 0; i < ring.pointCount; i++) {
        const auto &a = pts[i];
        const auto &b = pts[i + 1 == ring.pointCount ? 0 : i + 1];
        if ((a.lat <= sampleLat) == (b.lat <= sampleLat)) continue;
        if (n == kMaxCrossings) continue;
        const float lon = a.lon + (sampleLat - a.lat) / (b.lat - a.lat) * (b.lon - a.lon);
        crossings[n++] = v.x(lon);
      }
    }
    for (int i = 1; i < n; i++) {
      const float cur = crossings[i];
      int j = i - 1;
      while (j >= 0 && crossings[j] > cur) {
        crossings[j + 1] = crossings[j];
        j--;
      }
      crossings[j + 1] = cur;
    }
    uint16_t *row = fb + (size_t)py * v.w;
    for (int i = 0; i + 1 < n; i += 2) {
      int from = (int)ceilf(crossings[i]), to = (int)floorf(crossings[i + 1]);
      if (from < 0) from = 0;
      if (to >= v.w) to = v.w - 1;
      for (int px = from; px <= to; px++) row[px] = c;
    }
  }
}

}  // namespace quake_draw

namespace quake_draw {
inline bool regionContains(const QuakeRegions &r, const QuakeRegions::Region &rg, float lon, float lat) {
  if (lon < rg.minLon || lon > rg.maxLon || lat < rg.minLat || lat > rg.maxLat) return false;
  bool inside = false;
  for (int k = 0; k < rg.ringCount; k++) {
    const auto &ring = r.rings[rg.firstRing + k];
    const QuakeRegions::Point *pts = r.points + ring.firstPoint;
    for (int i = 0; i < ring.pointCount; i++) {
      const auto &a = pts[i];
      const auto &b = pts[i + 1 == ring.pointCount ? 0 : i + 1];
      if ((a.lat <= lat) != (b.lat <= lat) && lon < a.lon + (lat - a.lat) / (b.lat - a.lat) * (b.lon - a.lon)) inside = !inside;
    }
  }
  return inside;
}
}  // namespace quake_draw

// Sea, then regions tinted by their strongest intensity, then station squares weakest first so
// the strongest stay visible, then the epicentre.
inline QuakeMapView quakeRenderMap(const QuakeRegions &regions, const QuakeDetail &d, uint16_t *fb, int w, int h) {
  using namespace quake_draw;
  static constexpr uint32_t kSea = 0xD8E8F4, kLand = 0xF0F0E8, kOutline = 0x8A9090;
  const int kStationSize = d.stationCount > 300 ? 8 : 12;  // dense events would be solid otherwise
  const QuakeMapView v = quakeFitView(regions, d, w, h);
  rect(fb, w, h, 0, 0, w, h, swap565(kSea));

  // Database reports don't say which regions were affected, so work it out from their stations.
  static int8_t stationRank[512];
  if (d.areasAreStations) {
    memset(stationRank, 0, sizeof stationRank);
    for (int s = 0; s < d.stationCount; s++) {
      for (int i = 0; i < regions.regionCount && i < 512; i++) {
        if (d.stations[s].rank > stationRank[i] && regionContains(regions, regions.regions[i], d.stations[s].lon, d.stations[s].lat)) {
          stationRank[i] = d.stations[s].rank;
        }
      }
    }
  }
  for (int i = 0; i < regions.regionCount; i++) {
    const auto &rg = regions.regions[i];
    if (v.x(rg.maxLon) < 0 || v.x(rg.minLon) >= w || v.y(rg.minLat) < 0 || v.y(rg.maxLat) >= h) continue;
    int rank = d.areasAreStations && i < 512 ? stationRank[i] : 0;
    for (int a = 0; a < d.areaCount && !d.areasAreStations; a++) {
      if (d.areas[a].code == rg.code) rank = d.areas[a].rank;
    }
    const uint32_t color = rank > 0 ? mixWithWhite(quakeIntensityColor(rank), rank == 1 ? 100 : 45) : kLand;
    fillRegion(regions, rg, v, fb, swap565(color));
  }
  const uint16_t outline = swap565(kOutline);
  for (int i = 0; i < regions.regionCount; i++) {
    const auto &rg = regions.regions[i];
    if (v.x(rg.maxLon) < 0 || v.x(rg.minLon) >= w || v.y(rg.minLat) < 0 || v.y(rg.maxLat) >= h) continue;
    for (int k = 0; k < rg.ringCount; k++) {
      const auto &ring = regions.rings[rg.firstRing + k];
      const QuakeRegions::Point *pts = regions.points + ring.firstPoint;
      for (int p = 0; p < ring.pointCount; p++) {
        const auto &a = pts[p];
        const auto &b = pts[p + 1 == ring.pointCount ? 0 : p + 1];
        line(fb, w, h, v.x(a.lon), v.y(a.lat), v.x(b.lon), v.y(b.lat), outline);
      }
    }
  }
  const uint16_t edge = swap565(0x333333);
  for (int rank = 1; rank <= 9; rank++) {
    const uint16_t fill = swap565(quakeIntensityColor(rank));
    for (int i = 0; i < d.stationCount; i++) {
      const auto &s = d.stations[i];
      if (s.rank != rank) continue;
      const int cx = (int)v.x(s.lon), cy = (int)v.y(s.lat);
      rect(fb, w, h, cx - kStationSize / 2 - 1, cy - kStationSize / 2 - 1, kStationSize + 2, kStationSize + 2, edge);
      rect(fb, w, h, cx - kStationSize / 2, cy - kStationSize / 2, kStationSize, kStationSize, fill);
    }
  }
  if (d.hasCoordinate) {
    const float cx = v.x(d.lon), cy = v.y(d.lat);
    const uint16_t white = swap565(0xFFFFFF), red = swap565(0xE00000);
    for (int pass = 0; pass < 2; pass++) {
      const int s = 14, t = pass == 0 ? 7 : 3;  // small enough not to hide the strongest stations
      const uint16_t c = pass == 0 ? white : red;
      line(fb, w, h, cx - s, cy - s, cx + s, cy + s, c, t);
      line(fb, w, h, cx - s, cy + s, cx + s, cy - s, c, t);
    }
  }
  return v;
}
