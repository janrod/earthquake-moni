// Which older earthquakes the device holds, and what to ask JMA's long-term intensity database
// for next. Host-testable: no Arduino dependencies.
//
// The live list only covers about 30 days. Everything of intensity 3 and up is built into the
// firmware (quake_snapshot.h) and topped up from the database once the live list has moved past
// the snapshot, so those levels are complete back to 1919. Weaker ones are far too many, and are
// fetched on request, so how far back they're complete differs. Times are the sortable
// "2026-11-03T14:40:00".
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quake_data.h"

struct QuakeDbRequest {
  int minRank;
  bool topUp;  // new rows for the built-in levels, rather than older ones for a weak level
  char fromDate[11];  // "2011-03-01"
  char toDate[11];
  char toTime[6];  // "14:40"
};

class QuakeHistory {
 public:
  static constexpr const char *kRecordsStart = "1919-01-01T00:00:00";
  static constexpr int kDbRowLimit = 1000;  // the database returns the newest this many
  static constexpr int kPreloadRank = 3;    // built in from this level up
  // Reaching past the live list's end means a top-up lasts a while as that end moves on.
  static constexpr int kTopUpMarginDays = 7;
  // The database refuses a search past its newest day, about two days ago.
  static constexpr int kDbLagDays = 3;
  // A top-up must not hit the row limit, which would drop the oldest rows it was sent for.
  static constexpr int kTopUpStepDays = 90;

  void setPreloaded(const char *through) {
    snprintf(storeThrough_, sizeof storeThrough_, "%s", through);
    for (int r = 1; r <= 9; r++) snprintf(covered_[r], sizeof covered_[r], "%s", r < kPreloadRank ? through : kRecordsStart);
    known_ = true;
  }

  // A new live list, from its oldest and newest rows. Weak rows were fetched up to where the list
  // ended then, so if its end has moved on, there's a hole between them. True means they must
  // be dropped.
  bool liveLoaded(const char *oldestLive, const char *newestLive) {
    if (!oldestLive || !oldestLive[0]) return false;
    const bool gap = weakLoaded_ && strcmp(oldestLive, anchor_) > 0;
    if (!weakLoaded_ || gap) {
      for (int r = 1; r < kPreloadRank; r++) snprintf(covered_[r], sizeof covered_[r], "%s", oldestLive);
      snprintf(anchor_, sizeof anchor_, "%s", oldestLive);
      weakLoaded_ = false;
    }
    snprintf(oldestLive_, sizeof oldestLive_, "%s", oldestLive);
    snprintf(newestLive_, sizeof newestLive_, "%s", newestLive ? newestLive : oldestLive);
    known_ = true;
    joinUp();
    return gap;
  }

  bool known() const { return known_; }

  // How far back the list is complete for these levels; mask 0 means all of them.
  const char *coveredSince(unsigned mask) const {
    const char *newest = nullptr;
    for (int r = 1; r <= 9; r++) {
      if (selected(mask, r) && (!newest || strcmp(covered_[r], newest) > 0)) newest = covered_[r];
    }
    return newest ? newest : covered_[1];
  }

  // The weakest selected level that goes further back than the list as a whole, or 0. Rows older
  // than coveredSince() are only this level and stronger.
  int partialFromRank(unsigned mask) const {
    const char *complete = coveredSince(mask);
    for (int r = 1; r <= 9; r++) {
      if (selected(mask, r) && strcmp(covered_[r], complete) < 0) return r;
    }
    return 0;
  }

  bool canLoadOlder(unsigned mask) const { return known_ && strncmp(coveredSince(mask), kRecordsStart, 10) > 0; }

  // The live list has moved past the built-in rows, and the database can already fill the hole.
  bool needsTopUp() const {
    if (!oldestLive_[0] || strcmp(storeThrough_, oldestLive_) >= 0) return false;
    char to[20];
    snprintf(to, sizeof to, "%sT23:59:59", topUpPlan().toDate);
    return strcmp(to, storeThrough_) > 0;
  }

  // The top-up, when only built-in levels are selected; otherwise the weakest level selected.
  QuakeDbRequest plan(unsigned mask) const {
    int rank = 0;
    for (int r = kPreloadRank - 1; r >= 1; r--) {
      if (selected(mask, r)) rank = r;
    }
    if (rank == 0) return topUpPlan();
    QuakeDbRequest q = {};
    q.minRank = rank;
    static const int kMonths[] = {0, 1, 3};
    const char *to = coveredSince(mask);
    snprintf(q.toDate, sizeof q.toDate, "%.10s", to);
    snprintf(q.toTime, sizeof q.toTime, "%.5s", to + 11);
    const int months = atoi(to) * 12 + (atoi(to + 5) - 1) - kMonths[rank];
    const int year = months / 12, month = months % 12 + 1;
    const int day = atoi(to + 8) <= 28 ? atoi(to + 8) : 28;  // every month has a 28th
    if (year < 1919) snprintf(q.fromDate, sizeof q.fromDate, "1919-01-01");
    else snprintf(q.fromDate, sizeof q.fromDate, "%04d-%02d-%02d", year, month, day);
    return q;
  }

  // After a search. Levels at or above its minimum are now complete back to its start, or only
  // to the last row if the database hit its limit; a level with a gap above that stays put.
  // False means the rows are to be thrown away.
  bool applyResult(const QuakeDbRequest &q, const char *oldestRow, bool capped) {
    if (q.topUp) {
      if (capped) {  // again, for less time at once
        topUpStepDays_ = topUpStepDays_ > 4 ? topUpStepDays_ / 4 : 1;
        return false;
      }
      snprintf(storeThrough_, sizeof storeThrough_, "%sT23:59:59", q.toDate);
      joinUp();
      return true;
    }
    weakLoaded_ = true;
    char from[20];
    if (capped && oldestRow) snprintf(from, sizeof from, "%s", oldestRow);
    else snprintf(from, sizeof from, "%.10sT00:00:00", q.fromDate);
    char to[20];
    snprintf(to, sizeof to, "%sT%s:59", q.toDate, q.toTime);
    for (int r = q.minRank; r < kPreloadRank; r++) {
      if (strcmp(covered_[r], to) <= 0 && strcmp(from, covered_[r]) < 0) snprintf(covered_[r], sizeof covered_[r], "%s", from);
    }
    return true;
  }

  // The database's own code for "this level or stronger".
  static char dbLevelCode(int rank) { return "1234ABCD7"[rank >= 1 && rank <= 9 ? rank - 1 : 0]; }

  // Days since 1970 for "2026-10-04", and back.
  static int dayNumber(const char *date) {
    int y = atoi(date);
    const int m = atoi(date + 5), d = atoi(date + 8);
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
  }
  static void dateOfDay(int day, char *out, size_t cap) {
    int y, m, d;
    civilOfDay(day, &y, &m, &d);
    snprintf(out, cap, "%04d-%02d-%02d", y % 10000, m % 100, d % 100);
  }
  static void civilOfDay(int day, int *y, int *m, int *d) {
    day += 719468;
    const int era = (day >= 0 ? day : day - 146096) / 146097, doe = day - era * 146097;
    const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yoe + era * 400 + (*m <= 2);
  }

 private:
  static bool selected(unsigned mask, int r) { return mask == 0 || ((mask >> r) & 1); }

  QuakeDbRequest topUpPlan() const {
    QuakeDbRequest q = {};
    q.minRank = kPreloadRank;
    q.topUp = true;
    snprintf(q.fromDate, sizeof q.fromDate, "%.10s", storeThrough_);
    snprintf(q.toTime, sizeof q.toTime, "23:59");
    int to = dayNumber(q.fromDate) + topUpStepDays_;
    if (oldestLive_[0]) {
      if (dayNumber(oldestLive_) + kTopUpMarginDays < to) to = dayNumber(oldestLive_) + kTopUpMarginDays;
      if (dayNumber(newestLive_) - kDbLagDays < to) to = dayNumber(newestLive_) - kDbLagDays;
    }
    dateOfDay(to, q.toDate, sizeof q.toDate);
    return q;
  }

  // The built-in levels are complete back to the start of the records if they reach the live list.
  void joinUp() {
    const bool joined = strcmp(storeThrough_, oldestLive_) >= 0;
    for (int r = kPreloadRank; r <= 9; r++) snprintf(covered_[r], sizeof covered_[r], "%s", joined ? kRecordsStart : oldestLive_);
  }

  bool known_ = false, weakLoaded_ = false;
  int topUpStepDays_ = kTopUpStepDays;
  char covered_[10][20] = {};
  char anchor_[20] = {}, oldestLive_[20] = {}, newestLive_[20] = {}, storeThrough_[20] = {};
};

// "34 minutes ago" for a time like "2026-11-03T14:40:00", given the same clock's present in
// seconds since 1970. Empty for a time it can't read. A clock a little behind reads "just now".
inline void quakeAgoText(const char *t, int64_t now, char *out, size_t cap) {
  out[0] = 0;
  if (strlen(t) < 16) return;
  const int64_t then = (int64_t)QuakeHistory::dayNumber(t) * 86400 + atoi(t + 11) * 3600 + atoi(t + 14) * 60 + (strlen(t) >= 19 ? atoi(t + 17) : 0);
  const int64_t ago = now - then;
  if (ago < 60) {
    snprintf(out, cap, "just now");
    return;
  }
  const int days = (int)(ago / 86400);
  int n;
  const char *unit;
  if (ago < 3600) {
    n = (int)(ago / 60);
    unit = "minute";
  } else if (days < 1) {
    n = (int)(ago / 3600);
    unit = "hour";
  } else if (days < 30) {
    n = days;
    unit = "day";
  } else if (days < 365) {
    n = days / 30 > 11 ? 11 : days / 30;
    unit = "month";
  } else {
    n = (int)((int64_t)days * 100 / 36525);
    if (n < 1) n = 1;
    unit = "year";
  }
  snprintf(out, cap, "%d %s%s ago", n, unit, n == 1 ? "" : "s");
}

// Adds a batch to the older rows, both newest first, in place and without repeats. Returns the
// count. The caller makes sure it fits; a batch that doesn't is left out, as dropping the oldest
// rows would leave the list claiming to reach further back than it does.
inline int quakeMergeOlder(QuakeSummary *rows, int count, int cap, const QuakeSummary *batch, int n) {
  if (count + n > cap) return count;
  int i = count - 1, j = n - 1, k = count + n - 1;
  while (j >= 0) rows[k--] = i >= 0 && strcmp(rows[i].time, batch[j].time) <= 0 ? rows[i--] : batch[j--];
  int out = 0;
  for (int r = 0; r < count + n; r++) {
    bool repeat = false;
    for (int b = out - 1; b >= 0 && !repeat && strcmp(rows[b].time, rows[r].time) == 0; b--) repeat = strcmp(rows[b].eventId, rows[r].eventId) == 0;
    if (repeat) continue;
    if (out != r) rows[out] = rows[r];
    out++;
  }
  return out;
}

// Leaves only this level and stronger. Returns the count.
inline int quakeKeepFromRank(QuakeSummary *rows, int count, int rank) {
  int out = 0;
  for (int r = 0; r < count; r++) {
    if (rows[r].maxRank < rank) continue;
    if (out != r) rows[out] = rows[r];
    out++;
  }
  return out;
}

// The first older row that the live list doesn't already cover.
inline int quakeFirstOlderThan(const QuakeSummary *rows, int count, const char *time) {
  int lo = 0, hi = count;
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    if (strcmp(rows[mid].time, time) >= 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// A built-in earthquake, packed. Seconds count from 1919-01-01. The rank's top bit means the old
// scale's plain 5 or 6, and bits 4-5 are its QuakeTimePrecision. Latitude and longitude are
// hundredths, -32768 if unknown.
struct QuakeSnapshotRow {
  uint32_t seconds;
  int16_t lat100, lon100;
  uint16_t name;
  int16_t mag10;  // -1 unknown
  int16_t depthKm;  // -1 unknown
  uint8_t rank;
  int32_t idDelta;  // the event id is usually the time to the second, but not always
};

inline void quakeFromSnapshot(const QuakeSnapshotRow &s, const char *name, QuakeSummary *q) {
  memset(q, 0, sizeof *q);
  static const int kDays1919 = QuakeHistory::dayNumber("1919-01-01");
  const auto stamp = [](uint32_t seconds, const char *format, char *out, size_t cap) {
    int y, mo, d;
    QuakeHistory::civilOfDay(kDays1919 + (int)(seconds / 86400), &y, &mo, &d);
    const int sec = (int)(seconds % 86400);
    snprintf(out, cap, format, y % 10000, mo % 100, d % 100, sec / 3600 % 100, sec / 60 % 60, sec % 60);
  };
  stamp(s.seconds, "%04d-%02d-%02dT%02d:%02d:%02d", q->time, sizeof q->time);
  stamp((uint32_t)((int64_t)s.seconds + s.idDelta), "%04d%02d%02d%02d%02d%02d", q->eventId, sizeof q->eventId);
  snprintf(q->json, sizeof q->json, "db:%s", q->eventId);
  snprintf(q->name, sizeof q->name, "%s", name);
  if (s.mag10 >= 0) snprintf(q->magnitude, sizeof q->magnitude, "%d.%d", (s.mag10 / 10) % 100, s.mag10 % 10);
  q->depthKm = s.depthKm;
  q->maxRank = (int8_t)(s.rank & 0x0F);
  q->timePrecision = (int8_t)((s.rank >> 4) & 3);
  q->legacyScale = (s.rank & 0x80) != 0;
  q->hasCoordinate = s.lat100 != -32768;
  if (q->hasCoordinate) {
    q->lat = s.lat100 / 100.0f;
    q->lon = s.lon100 / 100.0f;
  }
  q->jsonHasStations = true;
}
