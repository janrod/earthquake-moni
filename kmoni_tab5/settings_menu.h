// Swipe-down settings panel: gesture handling and values only, no drawing.
// Host-testable: no Arduino dependencies.
#pragma once
#include <stdint.h>

class SettingsMenu {
 public:
  enum Button { kNone, kBrightMinus, kBrightPlus, kSoundMinus, kSoundPlus, kAutoMinus, kAutoPlus, kWifi, kLocation, kNight };
  struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };

  static constexpr int kPanelW = 720, kPanelH = 704;
  static constexpr int kRowY[3] = {18, 150, 282};
  static constexpr int kButtonW = 160, kButtonH = 112;
  static constexpr int kMinusX = 36, kPlusX = kPanelW - kButtonW - 36;

  static constexpr int kEdgeZone = 60;       // a swipe must start this close to the top
  static constexpr int kDragDecision = 100;  // travel needed to open or close on release
  static constexpr int kTapSlop = 24;
  static constexpr uint32_t kAutoCloseMs = 8000;  // the panel covers the clock and part of the map
  static constexpr int kSlidePxPerMs = 2;

  // Brightness stops at 10% so the screen can't be turned black. Sound 0 means off.
  static constexpr int kBrightMin = 10, kSoundMin = 0, kMax = 100, kStep = 10;

  static Rect buttonRect(Button b) {
    if (b == kWifi) return Rect{kMinusX, 414, kPanelW - 2 * kMinusX, 84};
    if (b == kLocation) return Rect{kMinusX, 506, kPanelW - 2 * kMinusX, 84};
    if (b == kNight) return Rect{kMinusX, 598, kPanelW - 2 * kMinusX, 84};
    const int row = (b == kAutoMinus || b == kAutoPlus) ? 2 : (b == kSoundMinus || b == kSoundPlus) ? 1 : 0;
    const int x = (b == kBrightMinus || b == kSoundMinus || b == kAutoMinus) ? kMinusX : kPlusX;
    return Rect{x, kRowY[row], kButtonW, kButtonH};
  }

  // Seconds before the earthquake pages return to the live page. 0 means they don't.
  static constexpr int kAutoStops[] = {0, 5, 10, 15, 20, 30, 60, 120};
  static constexpr int kAutoStopCount = 8;

  void begin(int brightness, int sound, int autoReturnSeconds = 10) {
    autoIndex_ = savedAutoIndex_ = 2;
    for (int i = 0; i < kAutoStopCount; i++) {
      if (kAutoStops[i] == autoReturnSeconds) autoIndex_ = savedAutoIndex_ = i;
    }
    brightness_ = savedBrightness_ = clampStep(brightness, kBrightMin);
    sound_ = savedSound_ = clampStep(sound, kSoundMin);
  }

  void update(bool pressed, int x, int y, uint32_t now) {
    const bool down = pressed && !wasPressed_;
    wasPressed_ = pressed;
    const uint32_t dt = now - lastNow_;
    lastNow_ = now;

    switch (phase_) {
      case Phase::Closed:
        if (down && y <= kEdgeZone) startDrag(x, y, 0, kNone, now);
        break;
      case Phase::Open:
        if (down) {
          if (y >= kPanelH) settleTo(0);  // tap outside
          else startDrag(x, y, kPanelH, buttonAt(x, y), now);
        } else if (now - lastActivity_ >= kAutoCloseMs) {
          settleTo(0);
        }
        break;
      case Phase::Dragging:
        lastActivity_ = now;
        if (pressed) {
          drag(x, y);
        } else {
          release();
        }
        break;
      case Phase::Settling: {
        const int step = (int)(dt > 500 ? 500 : dt) * kSlidePxPerMs;
        if (position_ < target_) position_ = position_ + step > target_ ? target_ : position_ + step;
        else position_ = position_ - step < target_ ? target_ : position_ - step;
        if (position_ == target_) finishSettle(now);
        break;
      }
    }
  }

  int position() const { return position_; }
  int brightness() const { return brightness_; }
  int sound() const { return sound_; }
  int autoReturnSeconds() const { return kAutoStops[autoIndex_]; }
  Button armed() const { return armed_; }
  // The finger or an animation is moving the panel: drawing should keep up with it.
  bool moving() const { return phase_ == Phase::Settling || (phase_ == Phase::Dragging && moved_); }

  bool canApply(Button b) const {
    switch (b) {
      case kBrightMinus: return brightness_ > kBrightMin;
      case kBrightPlus: return brightness_ < kMax;
      case kSoundMinus: return sound_ > kSoundMin;
      case kSoundPlus: return sound_ < kMax;
      case kAutoMinus: return autoIndex_ > 0;
      case kAutoPlus: return autoIndex_ < kAutoStopCount - 1;
      case kWifi:
      case kLocation:
      case kNight: return true;
      default: return false;
    }
  }

  bool takeLooksChanged() { return take(looksChanged_); }
  bool takeSoundChanged() { return take(soundChanged_); }
  bool takeWifiOpenRequest() { return take(wifiOpenRequest_); }
  bool takeLocationOpenRequest() { return take(locationOpenRequest_); }
  bool takeNightOpenRequest() { return take(nightOpenRequest_); }
  // Closes at once, for when another screen takes over.
  void closeNow() {
    position_ = target_ = 0;
    armed_ = kNone;
    finishSettle(lastNow_);
  }
  // Set once when the panel closes with new values, to limit flash writes.
  bool takeSaveRequest() { return take(saveRequest_); }

 private:
  enum class Phase { Closed, Dragging, Settling, Open };

  static bool take(bool &flag) {
    const bool v = flag;
    flag = false;
    return v;
  }
  static int clampStep(int v, int lo) {
    v = (v + kStep / 2) / kStep * kStep;
    return v < lo ? lo : v > kMax ? kMax : v;
  }
  static Button buttonAt(int x, int y) {
    for (Button b : {kBrightMinus, kBrightPlus, kSoundMinus, kSoundPlus, kAutoMinus, kAutoPlus, kWifi, kLocation, kNight}) {
      if (buttonRect(b).contains(x, y)) return b;
    }
    return kNone;
  }

  void startDrag(int x, int y, int fromPos, Button armed, uint32_t now) {
    phase_ = Phase::Dragging;
    grabX_ = x;
    grabY_ = y;
    grabPos_ = fromPos;
    moved_ = fromPos == 0;  // opening follows the finger at once; an open panel waits for the slop
    setArmed(armed);
    lastActivity_ = now;
  }

  void drag(int x, int y) {
    const int dx = x - grabX_, dy = y - grabY_;
    if (!moved_ && (dx > kTapSlop || dx < -kTapSlop || dy > kTapSlop || dy < -kTapSlop)) {
      moved_ = true;
      setArmed(kNone);
    }
    if (moved_) {
      const int p = grabPos_ + dy;
      position_ = p < 0 ? 0 : p > kPanelH ? kPanelH : p;
    }
  }

  void release() {
    if (armed_ != kNone) {
      apply(armed_);
      setArmed(kNone);
    }
    if (grabPos_ == 0) settleTo(position_ >= kDragDecision ? kPanelH : 0);
    else settleTo(kPanelH - position_ >= kDragDecision ? 0 : kPanelH);
  }

  void apply(Button b) {
    if (!canApply(b)) return;
    if (b == kWifi) {
      wifiOpenRequest_ = true;
      return;
    }
    if (b == kLocation) {
      locationOpenRequest_ = true;
      return;
    }
    if (b == kNight) {
      nightOpenRequest_ = true;
      return;
    }
    if (b == kAutoMinus || b == kAutoPlus) {
      autoIndex_ += b == kAutoPlus ? 1 : -1;
    } else if (b == kBrightMinus || b == kBrightPlus) {
      brightness_ += b == kBrightPlus ? kStep : -kStep;
    } else {
      sound_ += b == kSoundPlus ? kStep : -kStep;
      soundChanged_ = true;
    }
    looksChanged_ = true;
  }

  void setArmed(Button b) {
    if (armed_ != b) looksChanged_ = true;
    armed_ = b;
  }

  void settleTo(int target) {
    target_ = target;
    if (position_ == target) finishSettle(lastNow_);
    else phase_ = Phase::Settling;
  }

  void finishSettle(uint32_t now) {
    moved_ = false;
    if (target_ > 0) {
      phase_ = Phase::Open;
      lastActivity_ = now;
      return;
    }
    phase_ = Phase::Closed;
    if (brightness_ != savedBrightness_ || sound_ != savedSound_ || autoIndex_ != savedAutoIndex_) {
      savedBrightness_ = brightness_;
      savedSound_ = sound_;
      savedAutoIndex_ = autoIndex_;
      saveRequest_ = true;
    }
  }

  Phase phase_ = Phase::Closed;
  int position_ = 0, target_ = 0;
  int grabX_ = 0, grabY_ = 0, grabPos_ = 0;
  bool moved_ = false, wasPressed_ = false;
  Button armed_ = kNone;
  uint32_t lastActivity_ = 0, lastNow_ = 0;
  int brightness_ = 80, sound_ = 50, savedBrightness_ = 80, savedSound_ = 50;
  bool looksChanged_ = false, soundChanged_ = false, saveRequest_ = false;
  bool wifiOpenRequest_ = false, locationOpenRequest_ = false, nightOpenRequest_ = false;
  int autoIndex_ = 2, savedAutoIndex_ = 2;
};
