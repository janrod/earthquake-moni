// The battery level to show, for the Tab5.
// Host-testable: no Arduino dependencies.
//
// The battery's voltage is all there is to go on, and charging pushes it up. So on USB, charging
// is paused briefly once a minute for a clean reading. The shown level only falls on battery and
// only rises on USB, at a limited rate, so plugging in or unplugging doesn't make it jump.
//
// Unlike the StopWatch, the Tab5 can't see USB directly, but it can measure the battery's current:
// a battery powering the device is clearly discharging, one on USB isn't.
//
// The Tab5's battery is optional. With none fitted the charger's output swings wildly (8.4 V, 4.4 V
// across the pack), so readings on USB say nothing, but with charging paused it collapses (about
// 1.7 V). The pause reading therefore decides whether a battery is there.
#pragma once
#include <stdint.h>

class BatteryGauge {
 public:
  static constexpr uint32_t kSampleMs = 5000, kPauseEveryMs = 60000, kPauseMs = 3000;
  static constexpr float kDownPerMin = 1.0f, kUpPerMin = 2.0f;
  static constexpr float kExternalAboveMa = -100.0f;  // discharging harder than this means no USB
  static constexpr int kNoBatteryBelowMv = 2500;      // per cell

  enum class Action { kNone, kRead, kPauseCharging, kReadThenResume };

  // What the caller should do now. For either kind of read, it then calls reading().
  Action next(uint32_t now) {
    if (pausedAt_) return now - pausedAt_ >= kPauseMs ? Action::kReadThenResume : Action::kNone;
    if (haveLast_ && now - lastMs_ < kSampleMs) return Action::kNone;
    if (external_ && (!havePauseTime_ || now - lastPauseMs_ >= kPauseEveryMs) && haveLast_) {
      pausedAt_ = now ? now : 1;
      return Action::kPauseCharging;
    }
    return Action::kRead;
  }

  // mv is per cell. chargerOn is the charger's own status pin.
  void reading(uint32_t now, float mv, float mA, bool chargerOn, bool duringPause) {
    if (duringPause) {
      pausedAt_ = 0;
      lastPauseMs_ = resumedMs_ = now;
      havePauseTime_ = true;
      lastMs_ = now;
      haveLast_ = true;
      known_ = true;
      present_ = mv >= kNoBatteryBelowMv;
      if (!present_) {
        shown_ = -1;
        return;
      }
      if (mA > kExternalAboveMa) show(chargerRunning_ ? levelOf(mv) : 100, now);  // the charger stopped: full
      return;
    }
    lastMs_ = now;
    haveLast_ = true;
    const bool wasExternal = external_;
    external_ = mA > kExternalAboveMa;
    if (!external_) {  // only a real battery can be powering the device
      known_ = true;
      present_ = true;
    }
    // Just after a pause the charger's pin still says "not charging".
    if (!external_ || !havePauseTime_ || now - resumedMs_ > 1500) chargerRunning_ = external_ && chargerOn;
    if (external_) {
      if (!wasExternal) havePauseTime_ = false;  // plugged in again: measure at once
      return;
    }
    const float corrected = mv + loadMv_;
    if (wasExternal || average_ == 0.0f) average_ = corrected;  // just unplugged: start afresh
    average_ += (corrected - average_) * 0.1f;  // about 50 s at one sample per 5 s
    show(levelOf(average_), now);
  }

  void setLoadMv(float mv) { loadMv_ = mv; }
  // Whether a battery is fitted is only known after the first pause, some seconds after starting.
  bool known() const { return known_; }
  bool present() const { return present_; }
  bool hasLevel() const { return shown_ >= 0; }
  int level() const { return shown_ < 0 ? -1 : (int)(shown_ + 0.5f); }
  bool external() const { return external_; }
  bool charging() const { return chargerRunning_ && present_; }  // with no battery, the pin means nothing

  static int levelOf(float mv) {
    const int l = (int)((mv - 3300.0f) * 100.0f / 800.0f);
    return l < 0 ? 0 : l > 100 ? 100 : l;
  }

 private:
  void show(int level, uint32_t now) {
    const float budget = (now - shownMs_) / 60000.0f;
    shownMs_ = now;
    if (shown_ < 0) {
      shown_ = (float)level;
    } else if (external_ && level > shown_) {
      const float up = shown_ + kUpPerMin * budget;
      shown_ = up < level ? up : (float)level;
    } else if (!external_ && level < shown_) {
      const float down = shown_ - kDownPerMin * budget;
      shown_ = down > level ? down : (float)level;
    }
  }

  float shown_ = -1, average_ = 0, loadMv_ = 0;
  uint32_t lastMs_ = 0, pausedAt_ = 0, lastPauseMs_ = 0, resumedMs_ = 0, shownMs_ = 0;
  bool haveLast_ = false, havePauseTime_ = false;
  bool external_ = false, chargerRunning_ = false, present_ = false, known_ = false;
};
