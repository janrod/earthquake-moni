// Wi-Fi selector screens: network list, password keyboard, connecting. Touch handling
// and state only, no drawing or radio. Host-testable: no Arduino dependencies.
#pragma once
#include <stdint.h>
#include <string.h>

// WPA passphrases are 8-63 printable ASCII characters (0x20-0x7E), or exactly 64 hex digits.
enum class PasswordCheck { kTooShort, kOk, kBadHex };

inline PasswordCheck checkPassword(const char *pw) {
  const size_t n = strlen(pw);
  if (n < 8) return PasswordCheck::kTooShort;
  if (n < 64) return PasswordCheck::kOk;
  if (n > 64) return PasswordCheck::kBadHex;
  for (size_t i = 0; i < n; i++) {
    const char c = pw[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) return PasswordCheck::kBadHex;
  }
  return PasswordCheck::kOk;
}

class WifiSelector {
 public:
  enum class View { kHidden, kList, kPassword, kConnecting };
  enum Security { kOpen, kPsk, kUnsupported };
  struct Network {
    char ssid[33];
    int rssi;
    Security security;
    bool known;  // has connected before: joining needs no password
  };
  struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };

  enum KeyKind { kChar, kShift, kBackspace, kPage, kSpace, kConnect };
  struct Key {
    KeyKind kind;
    char ch;
    Rect r;
  };
  enum Page { kLower, kUpper, kSymbols };
  enum ShiftState { kShiftOff, kShiftOnce, kShiftLocked };

  static constexpr int kScreenW = 720, kScreenH = 1280;
  static constexpr int kMaxNetworks = 32;
  static constexpr int kMaxPassword = 64;
  static constexpr int kHeaderH = 100, kListTop = 110, kRowH = 100;
  static constexpr int kKeyW = 72, kKeyH = 130, kKeysTop = 620;
  static constexpr int kTapSlop = 24;
  // A row lights up only once the finger has stayed still this long, so starting a scroll
  // doesn't flash whichever row it began on. A quick tap still acts at once.
  static constexpr uint32_t kHighlightDelayMs = 120;
  static constexpr uint32_t kListTimeoutMs = 60 * 1000;      // the map must come back on its own
  static constexpr uint32_t kPasswordTimeoutMs = 120 * 1000;

  static constexpr Rect kBack{0, 0, 150, kHeaderH};
  static constexpr Rect kRescan{540, 0, 180, kHeaderH};
  static constexpr Rect kShow{570, 120, 130, 84};
  static constexpr Rect kField{20, 120, 540, 84};

  void open(uint32_t now) {
    wipePassword();
    view_ = View::kList;
    count_ = 0;
    scrollY_ = 0;
    failed_ = false;
    scanning_ = true;
    scanRequest_ = true;
    wasPressed_ = false;
    armedRow_ = -1;
    armedHeader_ = kHeaderNone;
    armedKey_ = -1;
    lastActivity_ = now;
    dirty_ = true;
  }

  void close() {
    wipePassword();
    view_ = View::kHidden;
    dirty_ = true;
  }

  void update(bool pressed, int x, int y, uint32_t now) {
    if (view_ == View::kHidden) return;
    const bool down = pressed && !wasPressed_;
    const bool up = !pressed && wasPressed_;
    wasPressed_ = pressed;
    if (pressed) lastActivity_ = now;
    if (down) pressedAt_ = now;

    switch (view_) {
      case View::kList:
        updateList(pressed, down, up, x, y);
        if (view_ == View::kList && now - lastActivity_ >= kListTimeoutMs) close();
        break;
      case View::kPassword:
        updatePassword(pressed, down, up, x, y);
        if (view_ == View::kPassword && now - lastActivity_ >= kPasswordTimeoutMs) close();
        break;
      default:
        break;  // connecting: waits for connectFinished()
    }
    if (view_ != View::kHidden && (up || down)) lastActivity_ = now;
    if (view_ == View::kList && pressed && armedRow_ >= 0 && !rowLit_ && now - pressedAt_ >= kHighlightDelayMs) {
      rowLit_ = true;
      dirty_ = true;
    }
  }

  // --- results from the network side ---
  void setNetworks(const Network *list, int count) {
    count_ = count > kMaxNetworks ? kMaxNetworks : count;
    if (count_ > 0) memcpy(networks_, list, sizeof(Network) * count_);
    scanning_ = false;
    scrollY_ = 0;
    armedRow_ = -1;
    dirty_ = true;
  }

  void connectFinished(bool ok, uint32_t now) {
    if (view_ != View::kConnecting) return;
    if (ok) {
      close();
      return;
    }
    failed_ = true;
    lastActivity_ = now;
    // An open network has no password to fix, so it goes back to the list.
    view_ = selected_.security == kOpen ? View::kList : View::kPassword;
    dirty_ = true;
  }

  void setConnectedSsid(const char *ssid) {
    if (strcmp(connectedSsid_, ssid) == 0) return;
    strncpy(connectedSsid_, ssid, sizeof connectedSsid_ - 1);
    dirty_ = true;
  }

  // --- requests for the network side ---
  bool takeScanRequest() { return take(scanRequest_); }
  bool takeConnectRequest(char *ssid, char *password) {
    if (!connectRequest_) return false;
    connectRequest_ = false;
    strcpy(ssid, selected_.ssid);
    strcpy(password, password_);
    return true;
  }

  // --- state for drawing ---
  bool takeDirty() { return take(dirty_); }
  View view() const { return view_; }
  bool visible() const { return view_ != View::kHidden; }
  int networkCount() const { return count_; }
  const Network &network(int i) const { return networks_[i]; }
  const Network &selected() const { return selected_; }
  bool isConnected(int i) const { return connectedSsid_[0] && strcmp(networks_[i].ssid, connectedSsid_) == 0; }
  int scrollY() const { return scrollY_; }
  bool scanning() const { return scanning_; }
  bool failed() const { return failed_; }
  int armedRow() const { return armedRow_; }
  int litRow() const { return rowLit_ ? armedRow_ : -1; }
  bool backArmed() const { return armedHeader_ == kHeaderBack; }
  bool rescanArmed() const { return armedHeader_ == kHeaderRescan; }
  bool showArmed() const { return armedHeader_ == kHeaderShow; }
  int armedKey() const { return armedKey_; }
  const char *password() const { return password_; }
  bool showPassword() const { return showPassword_; }
  Page page() const { return page_; }
  ShiftState shift() const { return shift_; }
  bool canConnect() const { return checkPassword(password_) == PasswordCheck::kOk; }

  static Rect rowRect(int i, int scrollY) { return Rect{0, kListTop + i * kRowH - scrollY, kScreenW, kRowH}; }

  // The keys of the current page.
  int keys(Key *out) const { return layoutKeys(page_, out); }
  static constexpr int kMaxKeys = 44;

  static int layoutKeys(Page page, Key *out) {
    int n = 0;
    auto row = [&](const char *chars, int x, int rowIndex) {
      for (; *chars; chars++, x += kKeyW) {
        out[n++] = Key{kChar, *chars, Rect{x, kKeysTop + rowIndex * kKeyH, kKeyW, kKeyH}};
      }
    };
    const int y3 = kKeysTop + 3 * kKeyH, y4 = kKeysTop + 4 * kKeyH;
    if (page == kSymbols) {
      row("!@#$%^&*()", 0, 0);
      row("-_=+[]{}\\|", 0, 1);
      row(";:'\",.<>/", 36, 2);
      row("`~?", 108, 3);
    } else {
      row("1234567890", 0, 0);
      row(page == kUpper ? "QWERTYUIOP" : "qwertyuiop", 0, 1);
      row(page == kUpper ? "ASDFGHJKL" : "asdfghjkl", 36, 2);
      row(page == kUpper ? "ZXCVBNM" : "zxcvbnm", 108, 3);
      out[n++] = Key{kShift, 0, Rect{0, y3, 108, kKeyH}};
    }
    out[n++] = Key{kBackspace, 0, Rect{612, y3, 108, kKeyH}};
    out[n++] = Key{kPage, 0, Rect{0, y4, 150, kKeyH}};
    out[n++] = Key{kSpace, ' ', Rect{150, y4, 330, kKeyH}};
    out[n++] = Key{kConnect, 0, Rect{480, y4, 240, kKeyH}};
    return n;
  }

 private:
  enum HeaderButton { kHeaderNone, kHeaderBack, kHeaderRescan, kHeaderShow };

  static bool take(bool &flag) {
    const bool v = flag;
    flag = false;
    return v;
  }

  void wipePassword() {
    memset(password_, 0, sizeof password_);
    showPassword_ = false;
  }

  void setArmedHeader(HeaderButton b) {
    if (armedHeader_ != b) dirty_ = true;
    armedHeader_ = b;
  }

  int maxScroll() const {
    const int content = count_ * kRowH, view = kScreenH - kListTop;
    return content > view ? content - view : 0;
  }

  void updateList(bool pressed, bool down, bool up, int x, int y) {
    if (down) {
      grabY_ = y;
      grabScroll_ = scrollY_;
      moved_ = false;
      inList_ = y >= kListTop;
      if (kBack.contains(x, y)) setArmedHeader(kHeaderBack);
      else if (kRescan.contains(x, y)) setArmedHeader(kHeaderRescan);
      else if (inList_) setArmedRow(rowAt(y));
    } else if (pressed) {
      const int dy = y - grabY_;
      if (dy > kTapSlop || dy < -kTapSlop) moved_ = true;
      if (moved_) {
        setArmedHeader(kHeaderNone);
        setArmedRow(-1);
        if (inList_) {
          int s = grabScroll_ - dy;
          s = s < 0 ? 0 : s > maxScroll() ? maxScroll() : s;
          if (s != scrollY_) dirty_ = true;
          scrollY_ = s;
        }
      }
    } else if (up) {
      const HeaderButton header = armedHeader_;
      const int row = armedRow_;
      setArmedHeader(kHeaderNone);
      setArmedRow(-1);
      if (header == kHeaderBack) {
        close();
      } else if (header == kHeaderRescan && !scanning_) {
        scanning_ = true;
        scanRequest_ = true;
        count_ = 0;
        failed_ = false;
        dirty_ = true;
      } else if (row >= 0) {
        choose(row);
      }
    }
  }

  void setArmedRow(int row) {
    if (row != armedRow_ && rowLit_) dirty_ = true;
    if (row != armedRow_) rowLit_ = false;
    armedRow_ = row;
  }

  int rowAt(int y) const {
    const int i = (y - kListTop + scrollY_) / kRowH;
    return i >= 0 && i < count_ ? i : -1;
  }

  void choose(int row) {
    const Network &n = networks_[row];
    if (n.security == kUnsupported) return;
    if (isConnected(row)) {  // already there: nothing to do, and no reason to drop it
      close();
      return;
    }
    selected_ = n;
    wipePassword();
    failed_ = false;
    dirty_ = true;
    page_ = kLower;
    shift_ = kShiftOff;
    if (n.security == kOpen || n.known) {
      // An empty password means "use the saved one, if any". If that fails,
      // connectFinished() brings up the keyboard.
      view_ = View::kConnecting;
      connectRequest_ = true;
    } else {
      view_ = View::kPassword;
    }
  }

  void setArmedKey(int k) {
    if (armedKey_ != k) dirty_ = true;
    armedKey_ = k;
  }

  void updatePassword(bool pressed, bool down, bool up, int x, int y) {
    Key k[kMaxKeys];
    const int n = keys(k);
    if (pressed) {
      if (down) {
        if (kBack.contains(x, y)) setArmedHeader(kHeaderBack);
        else if (kShow.contains(x, y)) setArmedHeader(kHeaderShow);
      }
      // The key under the finger fires on release, so a mistaken press can slide off.
      if (armedHeader_ == kHeaderNone) {
        int hit = -1;
        for (int i = 0; i < n; i++) {
          if (k[i].r.contains(x, y)) hit = i;
        }
        setArmedKey(hit);
      }
    } else if (up) {
      const HeaderButton header = armedHeader_;
      const int key = armedKey_;
      setArmedHeader(kHeaderNone);
      setArmedKey(-1);
      if (header == kHeaderBack) {
        wipePassword();
        failed_ = false;
        view_ = View::kList;
        dirty_ = true;
      } else if (header == kHeaderShow) {
        showPassword_ = !showPassword_;
        dirty_ = true;
      } else if (key >= 0) {
        press(k[key]);
      }
    }
  }

  void press(const Key &key) {
    dirty_ = true;
    failed_ = false;
    const size_t len = strlen(password_);
    switch (key.kind) {
      case kChar:
      case kSpace:
        if (len < (size_t)kMaxPassword) password_[len] = key.ch;
        if (key.kind == kChar && shift_ == kShiftOnce && isLetter(key.ch)) {
          shift_ = kShiftOff;
          page_ = kLower;
        }
        break;
      case kBackspace:
        if (len > 0) password_[len - 1] = 0;
        break;
      case kShift:
        shift_ = shift_ == kShiftOff ? kShiftOnce : shift_ == kShiftOnce ? kShiftLocked : kShiftOff;
        page_ = shift_ == kShiftOff ? kLower : kUpper;
        break;
      case kPage:
        page_ = page_ == kSymbols ? kLower : kSymbols;
        shift_ = kShiftOff;
        break;
      case kConnect:
        if (canConnect()) {
          view_ = View::kConnecting;
          connectRequest_ = true;
        }
        break;
    }
  }

  static bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

  View view_ = View::kHidden;
  Network networks_[kMaxNetworks];
  int count_ = 0;
  Network selected_ = {};
  char password_[kMaxPassword + 1] = {};
  char connectedSsid_[33] = {};
  bool showPassword_ = false;
  Page page_ = kLower;
  ShiftState shift_ = kShiftOff;
  int scrollY_ = 0, grabY_ = 0, grabScroll_ = 0;
  bool moved_ = false, inList_ = false, wasPressed_ = false;
  int armedRow_ = -1, armedKey_ = -1;
  bool rowLit_ = false;
  uint32_t pressedAt_ = 0;
  HeaderButton armedHeader_ = kHeaderNone;
  bool scanning_ = false, failed_ = false;
  bool dirty_ = false, scanRequest_ = false, connectRequest_ = false;
  uint32_t lastActivity_ = 0;
};
