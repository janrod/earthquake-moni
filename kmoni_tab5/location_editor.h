// The Location screen: two coordinate fields and a number pad. Touch handling and state only.
// Host-testable: no Arduino dependencies.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Accepts an optional minus, digits and at most one point. False for anything else or out of range.
inline bool parseCoordinate(const char *s, float limit, float *out) {
  const char *p = s;
  if (*p == '-') p++;
  int digits = 0, points = 0;
  for (; *p; p++) {
    if (*p == '.') points++;
    else if (*p >= '0' && *p <= '9') digits++;
    else return false;
  }
  if (digits == 0 || points > 1) return false;
  const float v = (float)atof(s);
  if (v < -limit || v > limit) return false;
  *out = v;
  return true;
}

class LocationEditor {
 public:
  struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  enum KeyKind { kDigit, kPoint, kMinus, kDelete, kClear, kSave };
  struct Key {
    KeyKind kind;
    char ch;
    Rect r;
    const char *label;
  };
  static constexpr int kKeyCount = 15, kKeyW = 240, kKeyH = 130, kKeysTop = 620;
  static constexpr int kMaxLen = 11;
  static constexpr uint32_t kTimeoutMs = 120 * 1000;
  static constexpr Rect kBack{0, 0, 150, 100};
  static Rect fieldRect(int field) { return Rect{20, field == 0 ? 160 : 340, 680, 100}; }

  static int keys(Key *out) {
    static const char *const digits = "123456789";
    int n = 0;
    for (int i = 0; i < 9; i++) {
      static char labels[9][2];
      labels[i][0] = digits[i];
      out[n++] = Key{kDigit, digits[i], Rect{i % 3 * kKeyW, kKeysTop + i / 3 * kKeyH, kKeyW, kKeyH}, labels[i]};
    }
    const int y3 = kKeysTop + 3 * kKeyH, y4 = kKeysTop + 4 * kKeyH;
    out[n++] = Key{kPoint, '.', Rect{0, y3, kKeyW, kKeyH}, "."};
    out[n++] = Key{kDigit, '0', Rect{kKeyW, y3, kKeyW, kKeyH}, "0"};
    out[n++] = Key{kDelete, 0, Rect{2 * kKeyW, y3, kKeyW, kKeyH}, "Del"};
    out[n++] = Key{kMinus, '-', Rect{0, y4, kKeyW, kKeyH}, "-"};
    out[n++] = Key{kClear, 0, Rect{kKeyW, y4, kKeyW, kKeyH}, "Clear"};
    out[n++] = Key{kSave, 0, Rect{2 * kKeyW, y4, kKeyW, kKeyH}, "Save"};
    return n;
  }

  void open(bool haveLocation, float lat, float lon, uint32_t now) {
    text_[0][0] = text_[1][0] = 0;
    if (haveLocation) {
      snprintf(text_[0], sizeof text_[0], "%.4f", lat);
      snprintf(text_[1], sizeof text_[1], "%.4f", lon);
    }
    active_ = 0;
    visible_ = true;
    wasPressed_ = false;
    armedKey_ = -1;
    armedField_ = -1;
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
        if (k[i].r.contains(x, y)) hit = i;
      }
      if (hit != armedKey_) dirty_ = true;
      armedKey_ = hit;
      if (down) {
        backArmed_ = kBack.contains(x, y);
        armedField_ = fieldRect(0).contains(x, y) ? 0 : fieldRect(1).contains(x, y) ? 1 : -1;
        dirty_ = true;
      }
    } else if (up) {
      const int key = armedKey_;
      const bool back = backArmed_;
      const int field = armedField_;
      armedKey_ = armedField_ = -1;
      backArmed_ = false;
      dirty_ = true;
      if (back) close();
      else if (field >= 0) active_ = field;
      else if (key >= 0) press(k[key]);
    } else if (now - lastActivity_ >= kTimeoutMs) {
      close();
    }
  }

  bool visible() const { return visible_; }
  int active() const { return active_; }
  const char *text(int field) const { return text_[field]; }
  int armedKey() const { return armedKey_; }
  bool backArmed() const { return backArmed_; }
  bool valid(int field) const {
    float v;
    return parseCoordinate(text_[field], field == 0 ? 90.0f : 180.0f, &v);
  }
  bool canSave() const { return valid(0) && valid(1); }
  bool takeDirty() { return take(dirty_); }
  bool takeCleared() { return take(cleared_); }
  bool takeSaved(float *lat, float *lon) {
    if (!saved_) return false;
    saved_ = false;
    parseCoordinate(text_[0], 90.0f, lat);
    parseCoordinate(text_[1], 180.0f, lon);
    return true;
  }

 private:
  static bool take(bool &flag) {
    const bool v = flag;
    flag = false;
    return v;
  }
  void press(const Key &key) {
    char *t = text_[active_];
    const size_t len = strlen(t);
    switch (key.kind) {
      case kDigit:
        if (len < (size_t)kMaxLen) t[len] = key.ch, t[len + 1] = 0;
        break;
      case kPoint:
        if (len < (size_t)kMaxLen && !strchr(t, '.')) t[len] = '.', t[len + 1] = 0;
        break;
      case kMinus:  // toggles the sign, wherever the cursor would be
        if (t[0] == '-') memmove(t, t + 1, len);
        else if (len < (size_t)kMaxLen) memmove(t + 1, t, len + 1), t[0] = '-';
        break;
      case kDelete:
        if (len > 0) t[len - 1] = 0;
        break;
      case kClear:
        text_[0][0] = text_[1][0] = 0;
        cleared_ = true;
        close();
        break;
      case kSave:
        if (canSave()) {
          saved_ = true;
          close();
        }
        break;
    }
  }

  bool visible_ = false, wasPressed_ = false, dirty_ = false, saved_ = false, cleared_ = false, backArmed_ = false;
  char text_[2][kMaxLen + 2] = {};
  int active_ = 0, armedKey_ = -1, armedField_ = -1;
  uint32_t lastActivity_ = 0;
};
