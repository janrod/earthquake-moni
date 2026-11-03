// Night mode: when the screen is switched off, and the screen for setting that up. Touch
// handling and state only. Host-testable: no Arduino dependencies.
#pragma once
#include <stdint.h>
#include <string.h>

struct NightSchedule {
  enum Mode { kOff, kScheduled, kContinuous };
  int mode = kOff;
  int start = 22 * 60, end = 4 * 60 + 30;  // minutes after midnight JST; may span midnight
  // How long the screen stays up after a touch. Also the hold for night dimming, with the mode Off.
  static constexpr int kWakeMin = 5, kWakeMax = 60, kWakeStep = 5;
  int wakeSeconds = 30;

  // minute is -1 until the clock is known. A schedule then leaves the screen on, since a wrong
  // guess could hide the map all day. Continuous doesn't need a clock.
  bool screenOff(int minute) const {
    if (mode == kContinuous) return true;
    if (mode != kScheduled || minute < 0 || start == end) return false;
    return start < end ? minute >= start && minute < end : minute >= start || minute < end;
  }

  // For values read back from flash.
  void normalize() {
    if (mode < kOff || mode > kContinuous) mode = kOff;
    if (start < 0 || start >= 24 * 60) start = 22 * 60;
    if (end < 0 || end >= 24 * 60) end = 4 * 60 + 30;
    if (wakeSeconds < kWakeMin || wakeSeconds > kWakeMax || wakeSeconds % kWakeStep != 0) wakeSeconds = 30;
  }
};

class NightModeEditor {
 public:
  struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  enum KeyKind { kDigit, kDelete, kSave };
  struct Key {
    KeyKind kind;
    char ch;
    Rect r;
    const char *label;
  };
  static constexpr int kKeyCount = 12, kKeyW = 240, kKeyH = 150, kKeysTop = 640;
  static constexpr uint32_t kTimeoutMs = 120 * 1000;
  static constexpr Rect kBack{0, 0, 150, 100};
  static Rect modeRect(int mode) { return Rect{20 + mode * 230, 120, 220, 90}; }
  static Rect fieldRect(int field) { return Rect{field == 0 ? 20 : 370, 270, 330, 110}; }
  static Rect wakeRect(bool plus) { return Rect{plus ? 590 : 280, 400, 110, 90}; }

  static int keys(Key *out) {
    static const char *const digits = "123456789";
    int n = 0;
    for (int i = 0; i < 9; i++) {
      out[n++] = Key{kDigit, digits[i], Rect{i % 3 * kKeyW, kKeysTop + i / 3 * kKeyH, kKeyW, kKeyH}, ""};
    }
    const int y3 = kKeysTop + 3 * kKeyH;
    out[n++] = Key{kDelete, 0, Rect{0, y3, kKeyW, kKeyH}, "Del"};
    out[n++] = Key{kDigit, '0', Rect{kKeyW, y3, kKeyW, kKeyH}, ""};
    out[n++] = Key{kSave, 0, Rect{2 * kKeyW, y3, kKeyW, kKeyH}, "Save"};
    return n;
  }

  void open(const NightSchedule &current, uint32_t now) {
    original_ = current;
    mode_ = current.mode;
    wake_ = current.wakeSeconds;
    setText(0, current.start);
    setText(1, current.end);
    active_ = 0;
    fresh_ = true;
    visible_ = true;
    wasPressed_ = false;
    armedKey_ = armedField_ = armedMode_ = armedWake_ = -1;
    backArmed_ = false;
    lastActivity_ = now;
    dirty_ = true;
  }
  void close() {
    visible_ = false;
    dirty_ = true;
  }

  void update(bool pressed, int x, int y, uint32_t now) {
    if (!visible_) return;
    const bool down = pressed && !wasPressed_;
    const bool up = !pressed && wasPressed_;
    wasPressed_ = pressed;
    if (pressed || up) lastActivity_ = now;
    Key k[kKeyCount];
    const int n = keys(k);
    if (pressed) {
      int hit = -1;
      for (int i = 0; i < n; i++) {
        if (k[i].r.contains(x, y) && keyEnabled(k[i])) hit = i;
      }
      if (hit != armedKey_) dirty_ = true;
      armedKey_ = hit;
      if (down) {
        backArmed_ = kBack.contains(x, y);
        armedField_ = !scheduled() ? -1 : fieldRect(0).contains(x, y) ? 0 : fieldRect(1).contains(x, y) ? 1 : -1;
        armedMode_ = -1;
        for (int m = 0; m < 3; m++) {
          if (modeRect(m).contains(x, y)) armedMode_ = m;
        }
        armedWake_ = wakeRect(false).contains(x, y) ? 0 : wakeRect(true).contains(x, y) ? 1 : -1;
        dirty_ = true;
      }
    } else if (up) {
      const int key = armedKey_, field = armedField_, mode = armedMode_, wake = armedWake_;
      const bool back = backArmed_;
      armedKey_ = armedField_ = armedMode_ = armedWake_ = -1;
      backArmed_ = false;
      dirty_ = true;
      if (back) {
        close();
      } else if (mode >= 0) {
        mode_ = mode;
      } else if (wake >= 0) {
        if (canWake(wake == 1)) wake_ += wake == 1 ? NightSchedule::kWakeStep : -NightSchedule::kWakeStep;
      } else if (field >= 0) {
        active_ = field;
        fresh_ = true;
      } else if (key >= 0) {
        press(k[key]);
      }
    } else if (now - lastActivity_ >= kTimeoutMs) {
      close();
    }
  }

  bool visible() const { return visible_; }
  int mode() const { return mode_; }
  int active() const { return active_; }
  int armedKey() const { return armedKey_; }
  int armedMode() const { return armedMode_; }
  int armedWake() const { return armedWake_; }
  int wakeSeconds() const { return wake_; }
  bool canWake(bool plus) const { return plus ? wake_ < NightSchedule::kWakeMax : wake_ > NightSchedule::kWakeMin; }
  bool backArmed() const { return backArmed_; }
  bool scheduled() const { return mode_ == NightSchedule::kScheduled; }
  bool keyEnabled(const Key &k) const { return k.kind == kSave ? canSave() : scheduled(); }

  // Minutes after midnight, or -1 for a time still being typed.
  int minutes(int field) const {
    const char *t = text_[field];
    if (strlen(t) < 4) return -1;
    return ((t[0] - '0') * 10 + (t[1] - '0')) * 60 + (t[2] - '0') * 10 + (t[3] - '0');
  }
  bool complete(int field) const { return minutes(field) >= 0; }
  // A schedule that starts as it ends would never switch the screen off.
  bool timesEqual() const { return complete(0) && minutes(0) == minutes(1); }
  bool canSave() const { return !scheduled() || (complete(0) && complete(1) && !timesEqual()); }

  // "22:00", or "2_:__" part way through typing.
  void display(int field, char out[6]) const {
    const char *t = text_[field];
    const size_t len = strlen(t);
    for (int i = 0, o = 0; i < 4; i++) {
      if (i == 2) out[o++] = ':';
      out[o++] = (size_t)i < len ? t[i] : '_';
    }
    out[5] = 0;
  }

  bool takeDirty() { return take(dirty_); }
  bool takeSaved(NightSchedule *out) {
    if (!saved_) return false;
    saved_ = false;
    *out = original_;
    out->mode = mode_;
    out->wakeSeconds = wake_;
    // Unused times may be half typed when the mode isn't Scheduled: those stay as they were.
    if (complete(0) && complete(1) && !timesEqual()) {
      out->start = minutes(0);
      out->end = minutes(1);
    }
    return true;
  }

 private:
  static bool take(bool &flag) {
    const bool v = flag;
    flag = false;
    return v;
  }
  void setText(int field, int minute) {
    const int h = minute / 60, m = minute % 60;
    text_[field][0] = (char)('0' + h / 10);
    text_[field][1] = (char)('0' + h % 10);
    text_[field][2] = (char)('0' + m / 10);
    text_[field][3] = (char)('0' + m % 10);
    text_[field][4] = 0;
  }
  // Refuses a digit no valid time could continue with, so only unfinished times can be wrong.
  static bool digitFits(const char *t, size_t len, char c) {
    switch (len) {
      case 0: return c <= '2';
      case 1: return t[0] < '2' || c <= '3';
      case 2: return c <= '5';
      default: return true;
    }
  }
  void press(const Key &key) {
    char *t = text_[active_];
    if (key.kind == kSave) {
      if (canSave()) {
        saved_ = true;
        close();
      }
    } else if (key.kind == kDelete) {
      fresh_ = false;
      const size_t len = strlen(t);
      if (len > 0) t[len - 1] = 0;
    } else {
      // The first digit in a field replaces what was there.
      const size_t len = fresh_ ? 0 : strlen(t);
      if (len >= 4 || !digitFits(t, len, key.ch)) return;
      t[len] = key.ch;
      t[len + 1] = 0;
      fresh_ = false;
      if (len == 3 && active_ == 0) {
        active_ = 1;
        fresh_ = true;
      }
    }
  }

  NightSchedule original_;
  bool visible_ = false, wasPressed_ = false, dirty_ = false, saved_ = false, backArmed_ = false, fresh_ = false;
  char text_[2][5] = {};
  int mode_ = NightSchedule::kOff, active_ = 0, armedKey_ = -1, armedField_ = -1, armedMode_ = -1, armedWake_ = -1;
  int wake_ = 30;
  uint32_t lastActivity_ = 0;
};
