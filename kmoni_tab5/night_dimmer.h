// Backlight level through the day: full set brightness in daylight, falling from sunset to
// the lowest level at 22:00, rising again from 04:30 to sunrise. A touch brings the set
// brightness back for a while, and an alert goes to full whatever the hour. Night mode asks for
// level 0, which the firmware takes to mean switching the panel off.
// Host-testable: no Arduino dependencies. Times are minutes after midnight JST.
#pragma once
#include <math.h>
#include <stdint.h>

struct SunTimes {
  int rise, set;
};

// NOAA's approximation, for the upper edge of the sun on the horizon. dayOfYear counts from
// 0, as tm_yday does. Polar day gives 0 and 1439; polar night gives noon for both.
inline SunTimes sunTimesJst(double lat, double lon, int dayOfYear) {
  const double kRad = M_PI / 180;
  const double g = 2 * M_PI * dayOfYear / 365;
  const double eqTime = 229.18 * (0.000075 + 0.001868 * cos(g) - 0.032077 * sin(g) - 0.014615 * cos(2 * g) - 0.040849 * sin(2 * g));
  const double decl = 0.006918 - 0.399912 * cos(g) + 0.070257 * sin(g) - 0.006758 * cos(2 * g) + 0.000907 * sin(2 * g) -
                      0.002697 * cos(3 * g) + 0.00148 * sin(3 * g);
  const double cosH = (cos(90.833 * kRad) - sin(lat * kRad) * sin(decl)) / (cos(lat * kRad) * cos(decl));
  if (cosH <= -1) return SunTimes{0, 1439};
  if (cosH >= 1) return SunTimes{720, 720};
  const double h = acos(cosH) / kRad;
  const auto jst = [&](double degrees) {
    const int m = (int)lround(720 - 4 * (lon + degrees) - eqTime + 9 * 60);
    return ((m % 1440) + 1440) % 1440;
  };
  return SunTimes{jst(h), jst(-h)};
}

class NightDimmer {
 public:
  static constexpr int kLowestPercent = 10;  // the same floor as the panel's Brightness
  static constexpr int kLowestFrom = 22 * 60, kLowestUntil = 4 * 60 + 30;
  // The sun doesn't always leave room: Tokyo's June sunrise is 04:25. The ramps run at least
  // this long, so the screen never jumps. It also keeps a far-away location harmless, since
  // the clock is always JST.
  static constexpr int kMinRampMinutes = 30;
  static constexpr uint32_t kTouchHoldMs = 30 * 1000;
  static constexpr float kFadeLevelsPerMs = 0.125f;  // about 2 s from full to lowest

  static int levelOf(int percent) { return percent * 255 / 100; }

  // The level wanted at this minute, for a set brightness.
  static int scheduled(int minute, SunTimes sun, int setPercent) {
    const int high = levelOf(setPercent), low = levelOf(kLowestPercent);
    if (high <= low) return high;
    if (minute >= kLowestFrom || minute < kLowestUntil) return low;
    const int morningEnd = clamp(sun.rise, kLowestUntil + kMinRampMinutes, 12 * 60);
    const int eveningStart = clamp(sun.set, 12 * 60, kLowestFrom - kMinRampMinutes);
    if (minute < morningEnd) return low + (high - low) * (minute - kLowestUntil) / (morningEnd - kLowestUntil);
    if (minute >= eveningStart) return high - (high - low) * (minute - eveningStart) / (kLowestFrom - eveningStart);
    return high;
  }

  // Called from every pass of the main loop. minute is -1 until the clock is known, which
  // keeps the set brightness: a wrong guess could leave the screen dim all day.
  int update(uint32_t now, bool pressed, bool alert, int minute, SunTimes sun, int setPercent, bool screenOff = false) {
    // A touch on a dark screen only wakes it: nobody can see what's under the finger.
    if (pressed && !wasPressed_ && shown_ >= 0 && (int)shown_ == 0) wakeTouch_ = true;
    if (!pressed) wakeTouch_ = false;
    wasPressed_ = pressed;
    if (pressed) {
      touched_ = true;
      lastTouchMs_ = now;
    }
    const bool held = touched_ && now - lastTouchMs_ < holdMs_;
    const int target = alert ? 255 : held ? levelOf(setPercent) : screenOff ? 0 : minute < 0 ? levelOf(setPercent) : scheduled(minute, sun, setPercent);
    if (shown_ < 0 || held || target >= shown_) {
      shown_ = (float)target;
    } else {
      shown_ -= (float)(now - lastNow_) * kFadeLevelsPerMs;
      if (shown_ < target) shown_ = (float)target;
    }
    lastNow_ = now;
    return (int)shown_;
  }

  void setHoldMs(uint32_t ms) { holdMs_ = ms; }
  // Keeps the screen on as a touch would, without one.
  void wake(uint32_t now) {
    touched_ = true;
    lastTouchMs_ = now;
  }
  // The current press began on a dark screen, so the pages shouldn't see it.
  bool wakeTouch() const { return wakeTouch_; }

 private:
  static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

  float shown_ = -1;
  bool touched_ = false, wasPressed_ = false, wakeTouch_ = false;
  uint32_t lastTouchMs_ = 0, lastNow_ = 0, holdMs_ = kTouchHoldMs;
};
