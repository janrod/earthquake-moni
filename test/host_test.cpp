// Host test: decode real kmoni GIFs and compare with macOS's decoder (sips), then
// render the map the way the firmware does. See test/run.sh.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

#include "../kmoni_tab5/battery_gauge.h"
#include "../kmoni_tab5/gif_layer.h"
#include "../kmoni_tab5/known_networks.h"
#include "../kmoni_tab5/location_editor.h"
#include "../kmoni_tab5/night_dimmer.h"
#include "../kmoni_tab5/night_mode.h"
#include "../kmoni_tab5/quake_history.h"
#include "../kmoni_tab5/quake_browser.h"
#include "../kmoni_tab5/quake_data.h"
#include "../kmoni_tab5/quake_map.h"
#include "../kmoni_tab5/quake_snapshot.h"
#include "../kmoni_tab5/resample.h"
#include "../kmoni_tab5/settings_menu.h"
#include "../kmoni_tab5/wifi_selector.h"

static std::vector<uint8_t> slurp(const std::string &p) {
  FILE *f = fopen(p.c_str(), "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", p.c_str()); exit(2); }
  std::vector<uint8_t> v;
  uint8_t b[4096];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
  fclose(f);
  return v;
}

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// argv: file.gif w h expected.bmp (24/32-bit, from sips). Transparent pixels render over white in
// neither decoder consistently, so only opaque pixels are compared.
static void compareWithBmp(const std::string &gif, int w, int h, const std::string &bmp) {
  auto data = slurp(gif);
  std::vector<uint32_t> px(w * h, 0);
  std::vector<uint8_t> work(w * h);
  CHECK(gifDrawOver(data.data(), data.size(), px.data(), w, h, work.data()));
  auto b = slurp(bmp);
  uint32_t off; int32_t bw, bh; uint16_t bpp;
  memcpy(&off, &b[10], 4); memcpy(&bw, &b[18], 4); memcpy(&bh, &b[22], 4); memcpy(&bpp, &b[28], 2);
  const bool topDown = bh < 0;
  if (topDown) bh = -bh;
  CHECK(bw == w && bh == h);
  const int stride = ((bw * bpp + 31) / 32) * 4;
  int compared = 0, diff = 0;
  for (int y = 0; y < h; y++) {
    const uint8_t *row = &b[off + (topDown ? y : h - 1 - y) * stride];
    for (int x = 0; x < w; x++) {
      const uint32_t p = px[y * w + x];
      if (!(p >> 24)) continue;
      const uint8_t *q = row + x * bpp / 8;
      compared++;
      if (((p >> 16) & 255) != q[2] || ((p >> 8) & 255) != q[1] || (p & 255) != q[0]) diff++;
    }
  }
  printf("%s: %d opaque px compared, %d differ\n", gif.c_str(), compared, diff);
  CHECK(compared > 0);
  CHECK(diff == 0);
}

static void testTruncated(const std::string &gif, int w, int h) {
  auto data = slurp(gif);
  std::vector<uint32_t> px(w * h, 0x12345678);
  std::vector<uint8_t> work(w * h);
  CHECK(!gifDrawOver(data.data(), data.size() / 2, px.data(), w, h, work.data()));
  CHECK(!gifDrawOver(data.data(), 20, px.data(), w, h, work.data()));
  for (auto p : px) if (p != 0x12345678) { CHECK(false && "dst touched on bad gif"); break; }
  // Wrong canvas size must be refused rather than overflow.
  CHECK(!gifDrawOver(data.data(), data.size(), px.data(), w - 1, h, work.data()));
}

static void testAxis() {
  std::vector<ResampleAxis> a(720);
  resampleAxis(a.data(), 352, 720);
  CHECK(a[0].i0 == 0 && a[0].w1 == 0);
  CHECK(a[719].i0 == 351 && a[719].i1 == 351);
  // Coverage of every source pixel must be near the 2.045 scale: no uneven squares.
  std::vector<double> cover(352, 0);
  for (auto &e : a) { cover[e.i0] += (256 - e.w1) / 256.0; cover[e.i1] += e.w1 / 256.0; }
  for (double c : cover) CHECK(c > 2.0 && c < 2.09);
}

static void testMix() {
  // Blending red with fully transparent must stay pure red rather than go dark.
  uint32_t m = resampleMix(0xFFFF0000, 0x00000000, 0xFFFF0000, 0x00000000, 128, 0);
  CHECK(((m >> 16) & 255) == 255 && (m & 0xFFFF) == 0);
  CHECK((m >> 24) > 120 && (m >> 24) < 135);
  CHECK(resampleMix(0, 0, 0, 0, 128, 128) == 0);
}

// Feeds touch samples at 10 ms intervals, like the UI loop.
struct MenuDriver {
  SettingsMenu m;
  uint32_t now = 1000;
  void step(bool p, int x, int y) { m.update(p, x, y, now += 10); }
  void idle(int ms) { for (int i = 0; i < ms / 10; i++) step(false, 0, 0); }
  void swipe(int x, int y0, int y1) {
    const int n = 10;
    for (int i = 0; i <= n; i++) step(true, x, y0 + (y1 - y0) * i / n);
    step(false, 0, 0);
    idle(400);
  }
  void tap(SettingsMenu::Button b) {
    auto r = SettingsMenu::buttonRect(b);
    for (int i = 0; i < 5; i++) step(true, r.x + r.w / 2, r.y + r.h / 2);
    step(false, 0, 0);
    idle(30);
  }
};

static void testMenu() {
  using M = SettingsMenu;
  {  // A long swipe from the top opens; a short one springs back.
    MenuDriver d; d.m.begin(80, 50);
    d.swipe(360, 10, 60);
    CHECK(d.m.position() == 0);
    d.swipe(360, 10, 300);
    CHECK(d.m.position() == M::kPanelH);
  }
  {  // A swipe starting mid-screen must do nothing, and neither must a press sliding up to the edge.
    MenuDriver d; d.m.begin(80, 50);
    d.swipe(360, 400, 800);
    CHECK(d.m.position() == 0);
    d.swipe(360, 400, 10);
    CHECK(d.m.position() == 0);
  }
  {  // The panel follows the finger while dragging.
    MenuDriver d; d.m.begin(80, 50);
    d.step(true, 360, 10); d.step(true, 360, 110);
    CHECK(d.m.position() == 100);
    CHECK(d.m.moving());
  }
  {  // Buttons change one value by one step, on release.
    MenuDriver d; d.m.begin(80, 50);
    d.swipe(360, 10, 300);
    auto r = M::buttonRect(M::kBrightPlus);
    d.step(true, r.x + 5, r.y + 5);
    CHECK(d.m.armed() == M::kBrightPlus);
    CHECK(d.m.brightness() == 80);
    d.step(false, 0, 0);
    CHECK(d.m.brightness() == 90 && d.m.sound() == 50);
    CHECK(!d.m.takeSoundChanged());
    d.tap(M::kSoundMinus);
    CHECK(d.m.sound() == 40 && d.m.brightness() == 90);
    CHECK(d.m.takeSoundChanged());
  }
  {  // Limits: brightness can't reach black, sound can reach off, neither passes 100.
    MenuDriver d; d.m.begin(80, 50);
    d.swipe(360, 10, 300);
    for (int i = 0; i < 15; i++) { d.tap(M::kBrightMinus); d.tap(M::kSoundMinus); }
    CHECK(d.m.brightness() == M::kBrightMin);
    CHECK(d.m.sound() == 0);
    CHECK(!d.m.canApply(M::kBrightMinus) && !d.m.canApply(M::kSoundMinus));
    for (int i = 0; i < 15; i++) { d.tap(M::kBrightPlus); d.tap(M::kSoundPlus); }
    CHECK(d.m.brightness() == 100 && d.m.sound() == 100);
  }
  {  // Swiping up from a button closes the panel without pressing it.
    MenuDriver d; d.m.begin(80, 50);
    d.swipe(360, 10, 300);
    auto r = M::buttonRect(M::kSoundPlus);
    d.swipe(r.x + 10, r.y + 50, r.y - 120);
    CHECK(d.m.position() == 0);
    CHECK(d.m.sound() == 50);
  }
  {  // A small upward drag on an open panel springs back open.
    MenuDriver d; d.m.begin(80, 50);
    d.swipe(360, 10, 300);
    d.swipe(360, 140, 90);
    CHECK(d.m.position() == M::kPanelH);
  }
  {  // Tapping outside closes without changing anything or saving.
    MenuDriver d; d.m.begin(80, 50);
    d.swipe(360, 10, 300);
    d.step(true, 360, 900); d.step(false, 0, 0); d.idle(400);
    CHECK(d.m.position() == 0);
    CHECK(!d.m.takeSaveRequest());
  }
  {  // Auto-closes when left alone; activity postpones it; saves once only after a change.
    MenuDriver d; d.m.begin(80, 50);
    d.swipe(360, 10, 300);
    d.idle(M::kAutoCloseMs - 2000);
    d.tap(M::kBrightPlus);
    d.idle(M::kAutoCloseMs - 2000);
    CHECK(d.m.position() == M::kPanelH);
    CHECK(!d.m.takeSaveRequest());
    d.idle(3000);
    CHECK(d.m.position() == 0);
    CHECK(d.m.takeSaveRequest());
    CHECK(!d.m.takeSaveRequest());
  }
  {  // Odd stored values are normalised.
    MenuDriver d; d.m.begin(3, 203);
    CHECK(d.m.brightness() == M::kBrightMin && d.m.sound() == 100);
  }
}

static void testMenuWifiButton() {
  using M = SettingsMenu;
  MenuDriver d; d.m.begin(80, 50);
  d.swipe(360, 10, 300);
  CHECK(d.m.position() == M::kPanelH);
  d.tap(M::kWifi);
  CHECK(d.m.takeWifiOpenRequest());
  CHECK(!d.m.takeWifiOpenRequest());
  CHECK(d.m.brightness() == 80 && d.m.sound() == 50);
  d.m.closeNow();
  CHECK(d.m.position() == 0);
  d.idle(100);
  CHECK(d.m.position() == 0);
  // Every control has to be inside the panel.
  for (auto b : {M::kBrightMinus, M::kBrightPlus, M::kSoundMinus, M::kSoundPlus, M::kWifi}) {
    auto r = M::buttonRect(b);
    CHECK(r.y >= 0 && r.y + r.h <= M::kPanelH && r.x >= 0 && r.x + r.w <= M::kPanelW);
  }
}

struct WifiDriver {
  WifiSelector w;
  uint32_t now = 1000;
  void step(bool p, int x, int y) { w.update(p, x, y, now += 10); }
  void tapAt(int x, int y) { step(true, x, y); step(true, x, y); step(false, 0, 0); }
  void tap(const WifiSelector::Rect &r) { tapAt(r.x + r.w / 2, r.y + r.h / 2); }
  bool tapKey(char c) {
    WifiSelector::Key k[WifiSelector::kMaxKeys];
    int n = w.keys(k);
    for (int i = 0; i < n; i++) {
      if (k[i].ch == c && (k[i].kind == WifiSelector::kChar || k[i].kind == WifiSelector::kSpace)) { tap(k[i].r); return true; }
    }
    return false;
  }
  void tapKind(WifiSelector::KeyKind kind) {
    WifiSelector::Key k[WifiSelector::kMaxKeys];
    int n = w.keys(k);
    for (int i = 0; i < n; i++) if (k[i].kind == kind) { tap(k[i].r); return; }
    CHECK(false && "key kind not on this page");
  }
  // Types any character, switching pages as a person would.
  bool type(char c) {
    if (tapKey(c)) return true;
    for (int tries = 0; tries < 4; tries++) {
      tapKind(WifiSelector::kPage);
      if (tapKey(c)) return true;
      if (w.page() != WifiSelector::kSymbols) {
        tapKind(WifiSelector::kShift);
        if (tapKey(c)) return true;
        if (w.shift() == WifiSelector::kShiftOnce) tapKind(WifiSelector::kShift);
        if (w.shift() == WifiSelector::kShiftLocked) tapKind(WifiSelector::kShift);
      }
    }
    return false;
  }
  void openWith(std::initializer_list<WifiSelector::Network> nets) {
    w.open(now);
    CHECK(w.takeScanRequest());
    std::vector<WifiSelector::Network> v(nets);
    w.setNetworks(v.data(), (int)v.size());
    w.takeDirty();
  }
  void tapRow(int i) { tap(WifiSelector::rowRect(i, w.scrollY())); }
};

static WifiSelector::Network net(const char *s, WifiSelector::Security sec) {
  WifiSelector::Network n = {};
  strcpy(n.ssid, s);
  n.rssi = -50;
  n.security = sec;
  return n;
}

static void testPasswordRules() {
  CHECK(checkPassword("") == PasswordCheck::kTooShort);
  CHECK(checkPassword("1234567") == PasswordCheck::kTooShort);
  CHECK(checkPassword("12345678") == PasswordCheck::kOk);
  CHECK(checkPassword("pass word with spaces") == PasswordCheck::kOk);
  std::string s63(63, 'z'), s64z(64, 'z'), s64h(64, 'a'), s65(65, 'a');
  CHECK(checkPassword(s63.c_str()) == PasswordCheck::kOk);
  CHECK(checkPassword(s64z.c_str()) == PasswordCheck::kBadHex);  // 64 is only a raw hex key
  CHECK(checkPassword(s64h.c_str()) == PasswordCheck::kOk);
  CHECK(checkPassword(s65.c_str()) == PasswordCheck::kBadHex);
}

// The keyboard must offer exactly the characters a WPA passphrase may contain.
static void testKeyboardCharacters() {
  using W = WifiSelector;
  bool seen[256] = {};
  for (auto page : {W::kLower, W::kUpper, W::kSymbols}) {
    W::Key k[W::kMaxKeys];
    const int n = W::layoutKeys(page, k);
    CHECK(n <= W::kMaxKeys);
    for (int i = 0; i < n; i++) {
      const auto &r = k[i].r;
      CHECK(r.x >= 0 && r.x + r.w <= W::kScreenW && r.y >= W::kKeysTop && r.y + r.h <= W::kScreenH);
      for (int j = 0; j < i; j++) {  // no overlapping keys
        const auto &o = k[j].r;
        CHECK(r.x >= o.x + o.w || o.x >= r.x + r.w || r.y >= o.y + o.h || o.y >= r.y + r.h);
      }
      if (k[i].kind == W::kChar || k[i].kind == W::kSpace) seen[(unsigned char)k[i].ch] = true;
    }
  }
  for (int c = 0; c < 256; c++) {
    if (seen[c] != (c >= 0x20 && c <= 0x7E)) { printf("char 0x%02X seen=%d\n", c, seen[c]); CHECK(false); }
  }
}

static void testWifiTyping() {
  using W = WifiSelector;
  WifiDriver d;
  d.openWith({net("Home", W::kPsk)});
  d.tapRow(0);
  CHECK(d.w.view() == W::View::kPassword);
  CHECK(strcmp(d.w.selected().ssid, "Home") == 0);
  // Every allowed character actually types, through touches on its key.
  std::string all;
  for (int c = 0x21; c <= 0x7E && all.size() < 64; c++) all += (char)c;
  for (char c : all) CHECK(d.type(c));
  CHECK(all == d.w.password());
  for (size_t i = 0; i < all.size(); i++) d.tapKind(W::kBackspace);
  CHECK(strlen(d.w.password()) == 0);
  std::string rest;
  for (int c = 0x7E; c >= 0x20 + 0 && rest.size() < 64; c--) rest += (char)c;
  for (char c : rest) CHECK(d.type(c));
  CHECK(rest == d.w.password());
  for (size_t i = 0; i < rest.size(); i++) d.tapKind(W::kBackspace);

  // Back to plain letters, wherever the character sweep ended.
  if (d.w.page() == W::kSymbols) d.tapKind(W::kPage);
  while (d.w.shift() != W::kShiftOff) d.tapKind(W::kShift);
  CHECK(d.w.page() == W::kLower);

  // Backspace on empty is harmless; input stops at the limit.
  d.tapKind(W::kBackspace);
  for (int i = 0; i < 70; i++) d.tapKey('a');
  CHECK(strlen(d.w.password()) == 64);
  CHECK(d.w.canConnect());  // 64 hex digits
  for (int i = 0; i < 64; i++) d.tapKind(W::kBackspace);

  // Shift: one letter, then back; second tap locks; third releases.
  d.tapKind(W::kShift);
  CHECK(d.w.page() == W::kUpper);
  CHECK(d.tapKey('A'));
  CHECK(d.w.page() == W::kLower);
  d.tapKind(W::kShift); d.tapKind(W::kShift);
  CHECK(d.w.shift() == W::kShiftLocked);
  CHECK(d.tapKey('B')); CHECK(d.tapKey('C'));
  CHECK(d.w.page() == W::kUpper);
  d.tapKind(W::kShift);
  CHECK(d.w.page() == W::kLower);
  CHECK(strcmp(d.w.password(), "ABC") == 0);

  // Sliding off a key before release types the key it ends on, and sliding off the keyboard types nothing.
  W::Key k[W::kMaxKeys]; int n = d.w.keys(k);
  d.step(true, k[0].r.x + 5, k[0].r.y + 5);
  d.step(true, 360, 300);
  d.step(false, 0, 0);
  CHECK(strcmp(d.w.password(), "ABC") == 0);
  (void)n;

  // Connect is refused until the password is valid.
  d.tapKind(W::kConnect);
  CHECK(d.w.view() == W::View::kPassword);
  for (char c : std::string("defgh")) d.tapKey(c);
  CHECK(strcmp(d.w.password(), "ABCdefgh") == 0);
  d.tapKind(W::kConnect);
  CHECK(d.w.view() == W::View::kConnecting);
  char ssid[33], pw[65];
  CHECK(d.w.takeConnectRequest(ssid, pw));
  CHECK(!d.w.takeConnectRequest(ssid, pw));
  CHECK(strcmp(ssid, "Home") == 0 && strcmp(pw, "ABCdefgh") == 0);

  // Failure keeps the typed password to fix; success closes and wipes it.
  d.w.connectFinished(false, d.now);
  CHECK(d.w.view() == W::View::kPassword && d.w.failed());
  CHECK(strcmp(d.w.password(), "ABCdefgh") == 0);
  d.tapKey('x');
  CHECK(!d.w.failed());
  d.tapKind(W::kConnect);
  d.w.takeConnectRequest(ssid, pw);
  d.w.connectFinished(true, d.now);
  CHECK(!d.w.visible());
  CHECK(strlen(d.w.password()) == 0);
}

static void testWifiList() {
  using W = WifiSelector;
  {  // Open connects directly; unsupported does nothing; back closes.
    WifiDriver d;
    d.openWith({net("Cafe", W::kOpen), net("Corp", W::kUnsupported), net("Home", W::kPsk)});
    d.tapRow(1);
    CHECK(d.w.view() == W::View::kList);
    d.tapRow(0);
    CHECK(d.w.view() == W::View::kConnecting);
    char ssid[33], pw[65];
    CHECK(d.w.takeConnectRequest(ssid, pw));
    CHECK(strcmp(ssid, "Cafe") == 0 && pw[0] == 0);
    d.w.connectFinished(false, d.now);
    CHECK(d.w.view() == W::View::kList && d.w.failed());
    d.tap(W::kBack);
    CHECK(!d.w.visible());
  }
  {  // Back from the keyboard wipes what was typed.
    WifiDriver d;
    d.openWith({net("Home", W::kPsk)});
    d.tapRow(0);
    d.tapKey('a');
    d.tap(W::kBack);
    CHECK(d.w.view() == W::View::kList);
    CHECK(strlen(d.w.password()) == 0);
  }
  {  // Rescan clears the list and asks again, and isn't repeated while scanning.
    WifiDriver d;
    d.openWith({net("Home", W::kPsk)});
    d.tap(W::kRescan);
    CHECK(d.w.takeScanRequest());
    CHECK(d.w.scanning() && d.w.networkCount() == 0);
    d.tap(W::kRescan);
    CHECK(!d.w.takeScanRequest());
  }
  {  // Dragging scrolls instead of selecting, and stops at the ends.
    WifiDriver d;
    std::vector<W::Network> many;
    for (int i = 0; i < 40; i++) many.push_back(net("N", W::kPsk));
    d.w.open(d.now);
    d.w.setNetworks(many.data(), (int)many.size());
    CHECK(d.w.networkCount() == W::kMaxNetworks);
    d.step(true, 360, 900); d.step(true, 360, 500); d.step(false, 0, 0);
    CHECK(d.w.scrollY() == 400);
    CHECK(d.w.view() == W::View::kList);
    d.step(true, 360, 300); d.step(true, 360, 1200); d.step(false, 0, 0);
    CHECK(d.w.scrollY() == 0);
    for (int i = 0; i < 10; i++) { d.step(true, 360, 1200); d.step(true, 360, 200); d.step(false, 0, 0); }
    CHECK(d.w.scrollY() == W::kMaxNetworks * W::kRowH - (W::kScreenH - W::kListTop));
    d.tapAt(360, 1270);  // last row after scrolling to the end
    CHECK(d.w.view() == W::View::kPassword);
  }
  {  // Left alone it returns to the map, sooner from the list than while typing.
    WifiDriver d;
    d.openWith({net("Home", W::kPsk)});
    for (uint32_t t = 0; t < W::kListTimeoutMs - 1000; t += 10) d.step(false, 0, 0);
    CHECK(d.w.visible());
    d.tapRow(0);
    for (uint32_t t = 0; t < W::kPasswordTimeoutMs - 1000; t += 10) d.step(false, 0, 0);
    CHECK(d.w.view() == W::View::kPassword);
    d.tapKey('a');
    for (uint32_t t = 0; t < W::kPasswordTimeoutMs - 1000; t += 10) d.step(false, 0, 0);
    CHECK(d.w.visible());
    for (uint32_t t = 0; t < 2000; t += 10) d.step(false, 0, 0);
    CHECK(!d.w.visible());
    CHECK(strlen(d.w.password()) == 0);
  }
  {  // Waiting on a connection never times out on its own.
    WifiDriver d;
    d.openWith({net("Home", WifiSelector::kOpen)});
    d.tapRow(0);
    for (uint32_t t = 0; t < 300000; t += 100) d.step(false, 0, 0);
    CHECK(d.w.view() == W::View::kConnecting);
    d.w.close();
    d.w.connectFinished(true, d.now);  // a late result after closing is ignored
    CHECK(!d.w.visible());
  }
}

static void testKnownNetworks() {
  KnownNetworks k;
  CHECK(k.count() == 0 && !k.find("A"));
  k.remember("A", "passwordA");
  k.remember("B", "passwordB");
  CHECK(k.count() == 2);
  CHECK(strcmp(k.find("A")->pass, "passwordA") == 0);
  k.remember("A", "changed!!");  // same SSID updates, and doesn't duplicate
  CHECK(k.count() == 2);
  CHECK(strcmp(k.find("A")->pass, "changed!!") == 0);
  CHECK(strcmp(k.find("B")->pass, "passwordB") == 0);
  // Full: the oldest goes, the newest stays; a seed never pushes anything out.
  char name[8];
  for (int i = 0; i < 8; i++) { snprintf(name, sizeof name, "N%d", i); k.remember(name, "password"); }
  CHECK(k.count() == KnownNetworks::kMax);
  CHECK(!k.find("A") && !k.find("B") && k.find("N7") && k.find("N0"));
  k.remember("N0", "again");  // re-saving the oldest when full loses nothing
  CHECK(k.count() == KnownNetworks::kMax && k.find("N7") && strcmp(k.find("N0")->pass, "again") == 0);
  k.seed("S", "x");
  CHECK(!k.find("S"));
  KnownNetworks e;
  e.seed("S", "p1"); e.seed("S", "p2");
  CHECK(e.count() == 1 && strcmp(e.find("S")->pass, "p1") == 0);
  // Longest legal values survive; junk from storage can't overrun.
  std::string ssid(32, 's'), pass(64, 'a');
  KnownNetworks l; l.remember(ssid.c_str(), pass.c_str());
  CHECK(l.find(ssid.c_str()) && pass == l.find(ssid.c_str())->pass);
  memset(l.data(), 'x', KnownNetworks::byteSize());
  l.normalize();
  CHECK(l.count() == KnownNetworks::kMax && strlen(l.find(std::string(32, 'x').c_str())->pass) == 64);
}

static void testSavedNetworkRows() {
  using W = WifiSelector;
  auto saved = net("Saved", W::kPsk); saved.known = true;
  char ssid[33], pw[65];
  {  // A saved network connects without the keyboard, asking for the stored password.
    WifiDriver d;
    d.openWith({saved, net("New", W::kPsk)});
    d.tapRow(0);
    CHECK(d.w.view() == W::View::kConnecting);
    CHECK(d.w.takeConnectRequest(ssid, pw));
    CHECK(strcmp(ssid, "Saved") == 0 && pw[0] == 0);
    d.w.connectFinished(true, d.now);
    CHECK(!d.w.visible());
  }
  {  // If that fails (say the password changed), the keyboard comes up to fix it.
    WifiDriver d;
    d.openWith({saved});
    d.tapRow(0);
    d.w.connectFinished(false, d.now);
    CHECK(d.w.view() == W::View::kPassword && d.w.failed());
    CHECK(d.w.page() == W::kLower);
  }
  {  // An unsaved secured network still asks.
    WifiDriver d;
    d.openWith({saved, net("New", W::kPsk)});
    d.tapRow(1);
    CHECK(d.w.view() == W::View::kPassword);
  }
  {  // The connected network just returns to the map, without reconnecting.
    WifiDriver d;
    d.openWith({saved, net("New", W::kPsk)});
    d.w.setConnectedSsid("Saved");
    CHECK(d.w.isConnected(0) && !d.w.isConnected(1));
    d.tapRow(0);
    CHECK(!d.w.visible());
    CHECK(!d.w.takeConnectRequest(ssid, pw));
  }
  {  // Saved but with no connection right now: connects.
    WifiDriver d;
    d.openWith({saved});
    d.w.setConnectedSsid("Saved");
    d.w.setConnectedSsid("");
    CHECK(!d.w.isConnected(0));
    d.tapRow(0);
    CHECK(d.w.view() == W::View::kConnecting);
  }
  {  // A long SSID is cut, not overrun; and an unsupported saved one stays disabled.
    WifiDriver d;
    auto corp = net("Corp", W::kUnsupported); corp.known = true;
    d.openWith({corp});
    d.tapRow(0);
    CHECK(d.w.view() == W::View::kList);
    d.w.setConnectedSsid(std::string(32, 'x').c_str());
  }
}

static void writePpm(const char *path, const uint16_t *fb, int w, int h) {
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6 %d %d 255\n", w, h);
  for (int i = 0; i < w * h; i++) {
    const uint16_t c = (uint16_t)(fb[i] << 8 | fb[i] >> 8);
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    fputc(r << 3 | r >> 2, f); fputc(g << 2 | g >> 4, f); fputc(b << 3 | b >> 2, f);
  }
  fclose(f);
}

static std::string slurpText(const std::string &p) {
  auto v = slurp(p);
  return std::string(v.begin(), v.end());
}

static void testQuakeHelpers() {
  CHECK(quakeIntensityRank("1") == 1 && quakeIntensityRank("4") == 4);
  CHECK(quakeIntensityRank("5-") == 5 && quakeIntensityRank("5+") == 6);
  CHECK(quakeIntensityRank("6-") == 7 && quakeIntensityRank("6+") == 8 && quakeIntensityRank("7") == 9);
  CHECK(quakeIntensityRank("!5-") == 5);
  CHECK(quakeIntensityRank("") == 0 && quakeIntensityRank(nullptr) == 0 && quakeIntensityRank("x") == 0);
  CHECK(strcmp(quakeIntensityLabel(6), "5強") == 0);
  float lat, lon; int depth;
  CHECK(quakeParseCoordinate("+35.7+140.6-50000/", &lat, &lon, &depth));
  CHECK(fabsf(lat - 35.7f) < 0.01f && fabsf(lon - 140.6f) < 0.01f && depth == 50);
  CHECK(quakeParseCoordinate("-12.5-170.25/", &lat, &lon, &depth));  // southern and western, no depth
  CHECK(fabsf(lat + 12.5f) < 0.01f && fabsf(lon + 170.25f) < 0.01f && depth == -1);
  CHECK(!quakeParseCoordinate("", &lat, &lon, &depth));
  CHECK(!quakeParseCoordinate(nullptr, &lat, &lon, &depth));
}

static void testQuakeListMerging() {
  // Newest first, as JMA serves it: a report with both parts, then the partial ones before it.
  const char *json = R"([
    {"ttl":"震源・震度情報","eid":"E1","ift":"発表","at":"2026-11-03T14:40:00+09:00","anm":"千葉県","cod":"+35.7+140.6-50000/","mag":"4.7","maxi":"3","json":"full.json"},
    {"ttl":"震源に関する情報","eid":"E1","ift":"発表","at":"2026-11-03T14:40:00+09:00","anm":"千葉県","cod":"+35.7+140.6-50000/","mag":"4.7","maxi":"","json":"hypo.json"},
    {"ttl":"震度速報","eid":"E1","ift":"発表","at":"2026-11-03T14:40:00+09:00","anm":"","cod":"","mag":"","maxi":"3","json":"sokuho.json"},
    {"ttl":"震度速報","eid":"E2","ift":"発表","at":"2026-11-03T13:00:00+09:00","anm":"","cod":"","mag":"","maxi":"2","json":"e2.json"},
    {"ttl":"南海トラフ地震関連解説情報","eid":"N1","ift":"発表","at":"2026-11-02T00:00:00+09:00","json":"nankai.json"},
    {"ttl":"震源・震度情報","eid":"C1","ift":"取消","at":"2026-11-01T00:00:00+09:00","json":"c2.json"},
    {"ttl":"震源・震度情報","eid":"C1","ift":"発表","at":"2026-11-01T00:00:00+09:00","anm":"x","json":"c1.json"},
    {"ttl":"遠地地震に関する情報","eid":"F1","ift":"発表","at":"2026-10-31T00:00:00+09:00","anm":"チリ","cod":"-30.0-71.0-30000/","mag":"7.0","maxi":"","json":"f.json"}
  ])";
  QuakeSummary rows[10];
  const int n = quakeParseList(json, strlen(json), rows, 10);
  CHECK(n == 3);
  CHECK(strcmp(rows[0].eventId, "E1") == 0 && strcmp(rows[0].json, "full.json") == 0);
  CHECK(rows[0].maxRank == 3 && rows[0].depthKm == 50 && rows[0].hasCoordinate);
  CHECK(strcmp(rows[0].time, "2026-11-03T14:40:00") == 0);
  CHECK(strcmp(rows[1].eventId, "E2") == 0 && !rows[1].hasCoordinate && rows[1].maxRank == 2);
  CHECK(strcmp(rows[2].eventId, "F1") == 0 && rows[2].lat < 0 && rows[2].maxRank == 0);
  CHECK(quakeParseList("not json", 8, rows, 10) == -1);
  CHECK(quakeParseList(json, strlen(json), rows, 2) == 2);  // capacity is respected
}

// Real JMA data, fetched by run.sh.
static void testQuakeRealData(const std::string &d) {
  static QuakeSummary rows[256];
  const auto listText = slurpText(d + "/quake_list.json");
  const int n = quakeParseList(listText.data(), listText.size(), rows, 256);
  printf("quake list: %d earthquakes from %zu bytes\n", n, listText.size());
  CHECK(n > 0);
  for (int i = 0; i < n; i++) {
    CHECK(rows[i].json[0] && rows[i].time[0]);
    for (int j = 0; j < i; j++) if (strcmp(rows[i].eventId, rows[j].eventId) == 0) { CHECK(false && "duplicate event"); break; }
  }
  for (int i = 1; i < n; i++) CHECK(strcmp(rows[i - 1].time, rows[i].time) >= 0);

  QuakeRegions regions;
  const auto geoText = slurpText(d + "/quake_geo.json");
  CHECK(quakeLoadRegions(geoText.data(), geoText.size(), &regions, malloc));
  printf("map: %d regions, %d rings, %d points\n", regions.regionCount, regions.ringCount, regions.pointCount);
  CHECK(regions.regionCount > 100 && regions.pointCount > 1000);

  static QuakeStation stations[4000];
  static QuakeArea areas[256];
  static uint16_t fb[720 * 740];
  for (const char *name : {"quake_detail1", "quake_detail2"}) {
    QuakeDetail q = {};
    q.stations = stations; q.stationCap = 4000; q.areas = areas; q.areaCap = 256;
    const auto text = slurpText(d + "/" + name + ".json");
    CHECK(quakeParseDetail(text.data(), text.size(), &q));
    printf("%s: %s M%s depth=%d max=%s stations=%d areas=%d\n", name, q.name, q.magnitude, q.depthKm,
           quakeIntensityLabel(q.maxRank), q.stationCount, q.areaCount);
    CHECK(q.stationCount > 0 && q.areaCount > 0 && q.maxRank > 0 && q.hasCoordinate);
    for (int i = 1; i < q.areaCount; i++) CHECK(q.areas[i - 1].rank >= q.areas[i].rank);

    const QuakeMapView v = quakeRenderMap(regions, q, fb, 720, 740);
    // The affected area must fill the view: everything inside it, and not lost in the middle.
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (int i = 0; i < q.stationCount; i++) {
      const float x = v.x(q.stations[i].lon), y = v.y(q.stations[i].lat);
      CHECK(x >= 0 && x < 720 && y >= 0 && y < 740);
      if (x < minX) minX = x;
      if (x > maxX) maxX = x;
      if (y < minY) minY = y;
      if (y > maxY) maxY = y;
    }
    CHECK(v.x(q.lon) >= 0 && v.x(q.lon) < 720 && v.y(q.lat) >= 0 && v.y(q.lat) < 740);
    printf("  stations span %.0fx%.0f px of 720x740, %.1f px/deg\n", maxX - minX, maxY - minY, v.pxPerDegLat);
    writePpm((d + "/" + name + ".ppm").c_str(), fb, 720, 740);
  }
}

static void testQuakeViewFallbacks() {
  QuakeRegions none;
  QuakeDetail q = {};
  // Nothing known: still a sane view.
  QuakeMapView v = quakeFitView(none, q, 720, 740);
  CHECK(v.pxPerDegLat > 0 && v.x(139.0f) > 0 && v.x(139.0f) < 720);
  // One point (a single station, or just an epicentre): zoom stops at the minimum span.
  q.hasCoordinate = true; q.lat = 35; q.lon = 140;
  v = quakeFitView(none, q, 720, 740);
  CHECK(fabsf(v.x(140) - 360) < 1 && fabsf(v.y(35) - 370) < 1);
  CHECK(v.pxPerDegLat <= 740 / 1.2f + 1);
  // A wide event is limited by the width rather than the height.
  QuakeStation s[2] = {{30, 125, 1}, {31, 145, 1}};
  q.stations = s; q.stationCount = 2;
  v = quakeFitView(none, q, 720, 740);
  CHECK(v.x(125) >= 0 && v.x(145) < 720);
}

struct QuakeDriver {
  QuakeBrowser b;
  std::vector<int32_t> rowStorage = std::vector<int32_t>(QuakeBrowser::kMaxRows + 2);
  QuakeDriver() { b.setRowStorage(rowStorage.data()); }
  uint32_t now = 1000;
  void step(bool p, int x, int y) { b.update(p, x, y, now += 10); }
  void tapAt(int x, int y) { step(true, x, y); step(true, x, y); step(false, 0, 0); }
  void drag(int x0, int y0, int x1, int y1) {
    for (int i = 0; i <= 10; i++) step(true, x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10);
    step(false, 0, 0);
  }
  void idle(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) step(false, 0, 0); }
};
static const char *testRowJson(int row) {
  static char buf[32];
  snprintf(buf, sizeof buf, "row%d.json", row);
  return buf;
}

static void testEdgeSwipe() {
  auto swipe = [](int x0, int y0, int x1, int y1) {
    EdgeSwipeDetector s;
    bool fired = false;
    for (int i = 0; i <= 10; i++) fired = s.update(true, x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10) || fired;
    return s.update(false, 0, 0) || fired;
  };
  CHECK(swipe(700, 600, 300, 610));   // from the right edge, leftward
  CHECK(!swipe(500, 600, 100, 600));  // leftward, but not from the edge
  CHECK(!swipe(100, 600, 500, 600));  // rightward
  CHECK(!swipe(700, 300, 700, 800));  // along the edge
  CHECK(!swipe(700, 20, 300, 40));    // from the top edge, which belongs to the settings panel
  EdgeSwipeDetector s;                // fires once, and says where the finger came down
  CHECK(!s.update(true, 700, 600));
  CHECK(s.update(true, 680, 600) && s.startX() == 700);
  CHECK(!s.update(true, 500, 600));
  s.update(false, 0, 0);
  CHECK(!s.update(true, 300, 600));   // a later press elsewhere starts over

  // The slide takes the finger from here, so the lift is never seen. The next swipe must still work.
  EdgeSwipeDetector again;
  again.update(true, 700, 600);
  CHECK(again.update(true, 680, 600));
  again.reset();
  CHECK(!again.update(true, 700, 600));
  CHECK(again.update(true, 680, 600));
}

static void testPageSlide() {
  auto settle = [](PageSlide &p, uint32_t &now) {
    for (int i = 0; i < 100 && p.active(); i++) p.update(false, 0, now += 10);
  };
  uint32_t now = 1000;
  PageSlide p;
  p.begin(0, 700, now);
  p.update(true, 600, now += 10);
  CHECK(p.position() == 100);  // the content follows the finger
  p.update(true, 1, now += 10);
  CHECK(p.position() == 699);
  p.update(true, -50, now += 10);
  CHECK(p.position() == 720);  // and stops at the end
  settle(p, now);
  CHECK(p.takeFinished() && !p.takeFinished() && p.listShown());

  PageSlide s;  // a short pull springs back
  s.begin(0, 700, now);
  s.update(true, 600, now += 10);
  settle(s, now);
  CHECK(s.takeFinished() && !s.listShown() && s.position() == 0);

  PageSlide b;  // and the way back: rightward from the list
  b.begin(PageSlide::kFull, 100, now);
  b.update(true, 400, now += 10);
  CHECK(b.position() == 420);
  settle(b, now);
  CHECK(!b.listShown() && b.position() == 0);

  PageSlide n;  // a short pull back stays on the list
  n.begin(PageSlide::kFull, 100, now);
  n.update(true, 200, now += 10);
  settle(n, now);
  CHECK(n.listShown());
}

static void testQuakeBrowser() {
  using B = QuakeBrowser;
  QuakeDriver d;
  d.b.setRowJsonLookup(testRowJson);
  d.b.setAutoReturnMs(0);
  d.b.open(d.now);
  CHECK(d.b.view() == B::View::kList);
  CHECK(d.b.takeListRequest());
  CHECK(!d.b.takeListRequest());
  CHECK(d.b.listLoad() == B::Load::kLoading);
  d.b.listLoaded(true, 40, d.now);
  CHECK(d.b.listLoad() == B::Load::kReady && d.b.rowCount() == 40);

  // No refetch until the server's data can have changed.
  d.idle(B::kRefreshMs - 1000);
  CHECK(!d.b.takeListRequest());
  d.idle(2000);
  CHECK(d.b.takeListRequest());
  CHECK(d.b.listLoad() == B::Load::kReady);  // the old rows stay up meanwhile
  d.b.listLoaded(false, 0, d.now);           // and through a failed refresh
  CHECK(d.b.listLoad() == B::Load::kReady && d.b.rowCount() == 40 && d.b.listRefreshFailed());

  // Scrolling doesn't select, and stops at the ends.
  d.drag(360, 900, 360, 500);
  CHECK(d.b.scrollY() == 400 && d.b.view() == B::View::kList);
  d.drag(360, 300, 360, 1200);
  CHECK(d.b.scrollY() == 0);
  for (int i = 0; i < 10; i++) d.drag(360, 1200, 360, 200);
  CHECK(d.b.scrollY() == (40 + 1) * B::kRowH - (B::kScreenH - B::kListTop - B::kFooterH));  // "Load older" is the last row
  d.drag(360, 300, 360, 1230);
  d.drag(360, 300, 360, 1230);
  d.drag(360, 300, 360, 1230);
  d.drag(360, 300, 360, 1230);
  d.drag(360, 300, 360, 1230);
  CHECK(d.b.scrollY() == 0);

  // Tap a row: detail opens and loads that row's report.
  auto r = B::rowRect(2, d.b.scrollY());
  d.tapAt(300, r.y + 20);
  CHECK(d.b.view() == B::View::kDetail && d.b.detailLoad() == B::Load::kLoading);
  char json[B::kJsonNameSize]; int row = -1;
  CHECK(d.b.takeDetailRequest(json, &row));
  CHECK(row == 2 && strcmp(json, "row2.json") == 0);
  d.b.detailLoaded(true);
  CHECK(d.b.detailLoad() == B::Load::kReady);

  // Back the way we came: swipe right from the detail to the list.
  d.drag(100, 600, 600, 600);
  CHECK(d.b.view() == B::View::kList);
  d.tapAt(300, r.y + 20);
  d.drag(600, 600, 100, 600);  // leftward does nothing here
  CHECK(d.b.view() == B::View::kDetail);
  d.tapAt(50, 50);  // the Back button
  CHECK(d.b.view() == B::View::kList);
  int finger = -1;
  CHECK(!d.b.takeBackDrag(&finger));

  // A rightward drag on the list hands over to the page slide. Leftward doesn't.
  d.drag(600, 600, 100, 600);
  CHECK(!d.b.takeBackDrag(&finger) && d.b.view() == B::View::kList);
  d.drag(100, 600, 600, 600);
  CHECK(d.b.takeBackDrag(&finger) && finger == 100);
  CHECK(!d.b.takeBackDrag(&finger));
  d.b.close();
  CHECK(!d.b.visible());

  // Tapping the footer or empty space does nothing.
  d.b.open(d.now);
  d.tapAt(300, B::kScreenH - 10);
  CHECK(d.b.view() == B::View::kList);
  d.b.listLoaded(true, 1, d.now);
  d.tapAt(300, 900);
  CHECK(d.b.view() == B::View::kList);
  d.b.close();

  // Auto-return, from either page; touching postpones it; off means never.
  QuakeDriver a;
  a.b.setRowJsonLookup(testRowJson);
  a.b.setAutoReturnMs(10000);
  a.b.open(a.now);
  a.b.listLoaded(true, 5, a.now);
  a.idle(9000);
  CHECK(a.b.visible());
  a.drag(360, 900, 360, 800);
  a.idle(9000);
  CHECK(a.b.visible());
  a.idle(1500);
  CHECK(!a.b.visible());
  a.b.open(a.now);
  auto r2 = B::rowRect(0, 0);
  a.tapAt(300, r2.y + 20);
  CHECK(a.b.view() == B::View::kDetail);
  a.idle(9000);
  CHECK(a.b.visible());
  a.idle(1500);
  CHECK(!a.b.visible());
  a.b.setAutoReturnMs(0);
  a.b.open(a.now);
  a.idle(600000);
  CHECK(a.b.visible());

  // A first load that failed retries soon, including on reopening.
  QuakeDriver f;
  f.b.setAutoReturnMs(0);
  f.b.open(f.now);
  CHECK(f.b.takeListRequest());
  f.b.listLoaded(false, 0, f.now);
  CHECK(f.b.listLoad() == B::Load::kFailed);
  CHECK(!f.b.takeListRequest());
  f.idle(B::kRetryMs + 100);
  CHECK(f.b.takeListRequest());
}

static int testRowRank(int row) {
  // Row: 0 none, 1 rank1, 2 rank2, 3 rank1, 4 rank5(5-), 5 rank6(5+), 6 none, 7 rank9(7), rest rank 3
  static const int ranks[] = {0, 1, 2, 1, 5, 6, 0, 9};
  return row < 8 ? ranks[row] : 3;
}

static void testQuakeFilter() {
  using B = QuakeBrowser;
  auto setup = [](QuakeDriver &d, int rows) {
    d.b.setRowJsonLookup(testRowJson);
    d.b.setRowRankLookup(testRowRank);
    d.b.setAutoReturnMs(0);
    d.b.open(d.now);
    d.b.takeListRequest();
    d.b.listLoaded(true, rows, d.now);
  };
  auto tapFilter = [](QuakeDriver &d, int button) {
    auto r = B::filterRect(button);
    d.tapAt(r.x + r.w / 2, r.y + r.h / 2);
  };
  QuakeDriver d;
  setup(d, 30);
  CHECK(d.b.visibleCount() == 30 && d.b.filterOn(0));
  for (int i = 0; i < B::kFilterCount; i++) {  // all ten fit the screen, without overlapping the list
    auto r = B::filterRect(i);
    CHECK(r.x >= 0 && r.x + r.w <= 720 && r.y >= B::kHeaderH && r.y + r.h <= B::kListTop);
  }
  tapFilter(d, 1);
  CHECK(d.b.filterOn(1) && !d.b.filterOn(0));
  CHECK(d.b.visibleCount() == 2 && d.b.visibleRow(0) == 1 && d.b.visibleRow(1) == 3);
  tapFilter(d, 5);  // toggles combine
  CHECK(d.b.visibleCount() == 3 && d.b.visibleRow(2) == 4);
  tapFilter(d, 6);
  CHECK(d.b.visibleCount() == 4 && d.b.visibleRow(3) == 5);
  tapFilter(d, 1);  // and turn back off
  CHECK(d.b.visibleCount() == 2 && d.b.visibleRow(0) == 4);
  tapFilter(d, 0);  // All clears them, and includes rows with no intensity
  CHECK(d.b.filterOn(0) && !d.b.filterOn(5) && d.b.visibleCount() == 30);
  tapFilter(d, 9);
  CHECK(d.b.visibleCount() == 1 && d.b.visibleRow(0) == 7);
  tapFilter(d, 9);  // clearing the last one means All
  CHECK(d.b.filterOn(0) && d.b.visibleCount() == 30);
  tapFilter(d, 8);  // a level with nothing in it
  CHECK(d.b.visibleCount() == 0 && d.b.listLoad() == B::Load::kReady);
  d.tapAt(300, 500);  // and an empty list is safe to tap
  CHECK(d.b.view() == B::View::kList);
  tapFilter(d, 8);

  // A tap opens the row it's on, by its real position in the list.
  tapFilter(d, 5);
  auto r = B::rowRect(0, d.b.scrollY());
  d.tapAt(300, r.y + 20);
  CHECK(d.b.view() == B::View::kDetail && d.b.selectedRow() == 4);
  char json[B::kJsonNameSize]; int row;
  CHECK(d.b.takeDetailRequest(json, &row) && strcmp(json, "row4.json") == 0 && row == 4);
  d.tapAt(50, 50);
  CHECK(d.b.view() == B::View::kList && d.b.filterOn(5));  // the filter is still set on return

  // It also survives a list refresh and leaving the pages.
  d.b.listLoaded(true, 30, d.now);
  CHECK(d.b.filterOn(5) && d.b.visibleCount() == 1);
  d.b.close();
  d.b.open(d.now);
  CHECK(d.b.filterOn(5));
  tapFilter(d, 0);

  // Changing the filter returns to the top; dragging across a button doesn't press it.
  QuakeDriver s;
  setup(s, 60);
  for (int i = 0; i < 3; i++) s.drag(360, 1100, 360, 300);
  CHECK(s.b.scrollY() > 0);
  tapFilter(s, 3);
  CHECK(s.b.scrollY() == 0 && s.b.visibleCount() == 52);
  auto f = B::filterRect(4);
  s.drag(f.x + 30, f.y + 30, f.x + 30, f.y + 400);
  CHECK(!s.b.filterOn(4) && s.b.armedFilter() == -1);
  s.drag(f.x + 30, f.y + 30, f.x + 200, f.y + 30);
  CHECK(!s.b.filterOn(4) && !s.b.filterOn(5) && !s.b.filterOn(6));

  // A row can be tapped after scrolling, and gives the row under the finger.
  QuakeDriver t;
  setup(t, 60);
  t.drag(360, 1100, 360, 300);
  CHECK(t.b.scrollY() == 800);
  const int want = 8;  // 800 / 118 = 6.8, so the second row showing is row 7, and the third row 8
  auto rr = B::rowRect(want, t.b.scrollY());
  CHECK(rr.y >= B::kListTop);
  t.tapAt(300, rr.y + 40);
  CHECK(t.b.view() == B::View::kDetail && t.b.selectedRow() == want);
}

static void testRowHighlightDelay() {
  using B = QuakeBrowser;
  auto setup = [](QuakeDriver &d) {
    d.b.setRowJsonLookup(testRowJson);
    d.b.setAutoReturnMs(0);
    d.b.open(d.now);
    d.b.listLoaded(true, 30, d.now);
    d.b.takeDirty();
  };
  const int y = B::rowRect(1, 0).y + 40;

  // Starting a scroll never lights the row, and doesn't even cause a redraw for it.
  QuakeDriver s;
  setup(s);
  s.step(true, 300, y);
  CHECK(s.b.armedRow() == 1 && s.b.litRow() == -1 && !s.b.takeDirty());
  s.step(true, 300, y - 10);
  CHECK(s.b.litRow() == -1 && !s.b.takeDirty());
  s.step(true, 300, y - 60);  // past the slop: it's a scroll now
  CHECK(s.b.armedRow() == -1);
  s.b.takeDirty();
  for (int i = 0; i < 30; i++) s.step(true, 300, y - 60);  // resting mid-scroll stays dark
  CHECK(s.b.litRow() == -1);
  s.step(false, 0, 0);
  CHECK(s.b.view() == B::View::kList);

  // Holding still lights it, once, after the delay. Moving away then turns it off with a redraw.
  QuakeDriver h;
  setup(h);
  h.step(true, 300, y);
  for (uint32_t t = 10; t < B::kHighlightDelayMs; t += 10) h.step(true, 300, y);
  CHECK(h.b.litRow() == -1);
  h.step(true, 300, y);
  h.step(true, 300, y);
  CHECK(h.b.litRow() == 1 && h.b.takeDirty());
  h.step(true, 300, y);
  CHECK(!h.b.takeDirty());
  h.step(true, 300, y + 80);
  CHECK(h.b.litRow() == -1 && h.b.takeDirty());
  h.step(false, 0, 0);
  CHECK(h.b.view() == B::View::kList);

  // A quick tap opens without ever lighting anything.
  QuakeDriver t;
  setup(t);
  t.step(true, 300, y);
  CHECK(t.b.litRow() == -1);
  t.step(false, 0, 0);
  CHECK(t.b.view() == B::View::kDetail && t.b.selectedRow() == 1);

  // A slower tap lights and still opens, and the next press starts dark.
  QuakeDriver l;
  setup(l);
  for (int i = 0; i < 20; i++) l.step(true, 300, y);
  CHECK(l.b.litRow() == 1);
  l.step(false, 0, 0);
  CHECK(l.b.view() == B::View::kDetail && l.b.litRow() == -1);

  // The Wi-Fi list behaves the same.
  WifiDriver w;
  w.openWith({net("A", WifiSelector::kPsk), net("B", WifiSelector::kPsk)});
  auto r = WifiSelector::rowRect(1, 0);
  w.step(true, 300, r.y + 20);
  CHECK(w.w.armedRow() == 1 && w.w.litRow() == -1 && !w.w.takeDirty());
  for (uint32_t ms = 0; ms < WifiSelector::kHighlightDelayMs + 20; ms += 10) w.step(true, 300, r.y + 20);
  CHECK(w.w.litRow() == 1 && w.w.takeDirty());
  w.step(false, 0, 0);
  CHECK(w.w.view() == WifiSelector::View::kPassword);
}

// Runs the gauge like the firmware does. `charge` adds what charging does to the voltage.
struct BatterySim {
  BatteryGauge g;
  uint32_t now = 1000;
  float cellMv = 3700, load = 0;
  bool usb = true;
  bool chargerWorks = true;
  int pauses = 0;
  bool paused = false;
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) {
      now += 100;
      const auto a = g.next(now);
      if (a == BatteryGauge::Action::kNone) continue;
      if (a == BatteryGauge::Action::kPauseCharging) {
        paused = true;
        pauses++;
        continue;
      }
      const bool charging = usb && !paused && chargerWorks;
      const float mv = usb ? (charging ? cellMv + 45 : cellMv) : cellMv - load;
      const float mA = usb ? (charging ? 800.0f : 0.0f) : -600.0f;
      const bool during = a == BatteryGauge::Action::kReadThenResume;
      g.reading(now, mv, mA, charging, during);
      if (during) paused = false;
    }
  }
};

static void testBatteryGauge() {
  using G = BatteryGauge;
  CHECK(G::levelOf(3300) == 0 && G::levelOf(4100) == 100 && G::levelOf(3700) == 50);
  CHECK(G::levelOf(2000) == 0 && G::levelOf(4300) == 100);

  // On USB: pauses to read, so charging's lift doesn't count, and shows the true level.
  BatterySim u;
  CHECK(u.g.level() == -1);
  u.run(30000);
  CHECK(u.g.external() && u.g.charging());
  CHECK(u.pauses >= 1 && !u.paused);
  CHECK(u.g.level() == 50);
  const int before = u.pauses;
  u.run(5 * 60000);
  CHECK(u.pauses - before >= 4 && u.pauses - before <= 6);  // about once a minute
  CHECK(u.g.level() == 50);

  // Rising is limited in speed on USB: a reading of 90% doesn't appear at once.
  u.cellMv = 4020;
  u.run(70000);
  CHECK(u.g.level() >= 51 && u.g.level() <= 56);
  u.run(30 * 60000);
  CHECK(u.g.level() == 90);

  // The charger stopping means full.
  u.chargerWorks = false;
  u.run(70000);
  CHECK(!u.g.charging());
  u.run(10 * 60000);
  CHECK(u.g.level() == 100);

  // Unplugging must not make it jump, even though the voltage drops under load.
  BatterySim p;
  p.cellMv = 3700;
  p.run(70000);
  CHECK(p.g.level() == 50);
  p.usb = false;
  p.load = 60;
  p.run(60000);
  CHECK(!p.g.external() && !p.g.charging());
  CHECK(p.g.level() >= 44 && p.g.level() <= 50);  // it may fall, but at most 1% a minute...
  p.g.setLoadMv(60);
  // ...and correcting for the load, it holds, with no pausing on battery.
  BatterySim q;
  q.cellMv = 3700;
  q.g.setLoadMv(60);
  q.usb = false;
  q.load = 60;
  q.run(10 * 60000);
  CHECK(q.g.level() == 50 && q.pauses == 0 && !q.paused);

  // On battery it falls with the battery, never rises; plugging in then doesn't jump either.
  q.cellMv = 3600;
  q.run(60000);
  CHECK(q.g.level() >= 45);
  q.run(30 * 60000);
  CHECK(q.g.level() == 37);
  q.cellMv = 3700;
  q.run(10 * 60000);
  CHECK(q.g.level() == 37);  // a recovering reading doesn't raise it on battery
  q.usb = true;
  q.run(4000);
  CHECK(q.g.level() == 37);
  q.run(70000);
  CHECK(q.g.external() && q.g.level() >= 38 && q.g.level() <= 42);

  // Unplugged mid-pause: the reading isn't trusted as a USB one.
  BatterySim m;
  m.run(70000);
  const int lvl = m.g.level();
  m.cellMv = 3400;
  for (int i = 0; i < 700 && !m.paused; i++) m.run(100);
  m.usb = false;
  m.run(4000);
  CHECK(!m.paused && m.g.level() <= lvl);

  // No battery fitted, with the readings a real Tab5 gave: wild on USB, collapsing when paused.
  G n;
  uint32_t t = 1000;
  static const float swings[] = {2206, 4189, 2200, 4190, 4189, 2202, 2606};
  CHECK(!n.known());
  int sawPause = 0;
  for (int i = 0; i < 2000; i++) {
    t += 100;
    const auto a = n.next(t);
    if (a == G::Action::kNone || a == G::Action::kPauseCharging) continue;
    const bool during = a == G::Action::kReadThenResume;
    n.reading(t, during ? 880 : swings[i % 7], 0, true, during);
    if (during) {
      sawPause++;
      CHECK(n.known() && !n.present());
    }
    CHECK(n.external() && !n.charging() && !n.hasLevel());  // never a level, and never "charging"
  }
  CHECK(sawPause >= 2);

  // A battery fitted later shows up at the next pause, with a true reading.
  for (int i = 0; i < 2000 && !n.present(); i++) {
    t += 100;
    const auto a = n.next(t);
    if (a == G::Action::kNone || a == G::Action::kPauseCharging) continue;
    const bool during = a == G::Action::kReadThenResume;
    n.reading(t, during ? 3700 : 3745, 800, true, during);
  }
  CHECK(n.present() && n.hasLevel() && n.level() == 50);

  // Running on a battery with no pause yet: that alone proves there's one.
  G b;
  b.reading(1000, 3700, -600, false, false);
  CHECK(b.known() && b.present() && !b.external() && b.hasLevel());
}

static void testRankAndDistance() {
  bool legacy = true;
  CHECK(quakeRankFromJapanese("震度５弱", &legacy) == 5 && !legacy);
  CHECK(quakeRankFromJapanese("震度５強") == 6 && quakeRankFromJapanese("震度６弱") == 7);
  CHECK(quakeRankFromJapanese("震度６強") == 8 && quakeRankFromJapanese("震度７") == 9 && quakeRankFromJapanese("震度１") == 1);
  CHECK(quakeRankFromJapanese("震度５", &legacy) == 5 && legacy);  // the old scale
  CHECK(quakeRankFromJapanese("震度６", &legacy) == 7 && legacy);
  CHECK(quakeRankFromJapanese("") == 0 && quakeRankFromJapanese(nullptr) == 0);
  CHECK(strcmp(quakeIntensityLabel(5, true), "5") == 0 && strcmp(quakeIntensityLabel(7, true), "6") == 0);
  CHECK(strcmp(quakeIntensityLabel(5, false), "5弱") == 0 && strcmp(quakeIntensityLabel(6, true), "5強") == 0);

  // Tokyo Station to known places, within a percent of published straight-line distances.
  const float lat = 35.6812f, lon = 139.7671f;
  CHECK(quakeDistanceKm(lat, lon, lat, lon) < 0.01f);
  const float osaka = quakeDistanceKm(lat, lon, 34.70f, 135.50f);  // Osaka
  printf("Tokyo to Osaka %.0f km\n", osaka);
  CHECK(osaka > 395 && osaka < 410);
  const float sapporo = quakeDistanceKm(lat, lon, 43.06f, 141.35f);  // Sapporo
  printf("Tokyo to Sapporo %.0f km\n", sapporo);
  CHECK(sapporo > 820 && sapporo < 845);
  CHECK(fabsf(quakeDistanceKm(0, 0, 0, 180) - 20015) < 20);  // halfway round the world
}

static QuakeSummary dbRow(const char *id, const char *time, int rank = 1) {
  QuakeSummary q = {};
  snprintf(q.eventId, sizeof q.eventId, "%s", id);
  snprintf(q.time, sizeof q.time, "%s", time);
  q.maxRank = (int8_t)rank;
  return q;
}

static void testQuakeHistory() {
  const unsigned only1 = 1u << 1, only2 = 1u << 2, only3 = 1u << 3, only7 = 1u << 9;
  const char *const oldest = "2026-10-04T20:03:00", *const newest = "2026-11-03T14:40:00";
  CHECK(QuakeHistory::dbLevelCode(1) == '1' && QuakeHistory::dbLevelCode(3) == '3' && QuakeHistory::dbLevelCode(5) == 'A' && QuakeHistory::dbLevelCode(9) == '7');

  // Day arithmetic, across every kind of boundary.
  for (const char *date : {"1919-01-01", "1970-01-01", "2000-02-29", "2024-12-31", "2026-03-01", "2026-11-03"}) {
    char back[11];
    QuakeHistory::dateOfDay(QuakeHistory::dayNumber(date), back, sizeof back);
    CHECK(strcmp(back, date) == 0);
  }
  CHECK(QuakeHistory::dayNumber("1970-01-02") == 1 && QuakeHistory::dayNumber("2024-03-01") - QuakeHistory::dayNumber("2024-02-28") == 2);

  // Before any download: the built-in levels are complete, the weak ones stop where the snapshot does.
  QuakeHistory h;
  CHECK(!h.known() && !h.canLoadOlder(0));
  h.setPreloaded("2026-11-01T00:00:00");
  CHECK(h.known() && !h.needsTopUp());
  CHECK(strcmp(h.coveredSince(only7), QuakeHistory::kRecordsStart) == 0 && strcmp(h.coveredSince(only3), QuakeHistory::kRecordsStart) == 0);
  CHECK(strcmp(h.coveredSince(only1), "2026-11-01T00:00:00") == 0);

  // The live list joins the snapshot: turning on any strong level needs no load at all.
  CHECK(!h.liveLoaded(oldest, newest));
  CHECK(!h.needsTopUp());
  CHECK(strcmp(h.coveredSince(only7), QuakeHistory::kRecordsStart) == 0 && !h.canLoadOlder(only7) && !h.canLoadOlder(only3 | only7));
  CHECK(strcmp(h.coveredSince(only1), oldest) == 0 && h.canLoadOlder(only1) && h.canLoadOlder(0));
  CHECK(strcmp(h.coveredSince(0), oldest) == 0);

  // What's below the complete point: the weakest level selected that reaches further.
  CHECK(h.partialFromRank(0) == 3);
  CHECK(h.partialFromRank(only1 | only7) == 9);
  CHECK(h.partialFromRank(only1 | (1u << 5) | only7) == 5);
  CHECK(h.partialFromRank(only7) == 0 && h.partialFromRank(only3 | only7) == 0 && h.partialFromRank(only1) == 0);

  // Weak levels go back by the month from the end of the live list, and only they gain.
  QuakeDbRequest q = h.plan(0);
  CHECK(q.minRank == 1 && !q.topUp && strcmp(q.fromDate, "2026-09-04") == 0 && strcmp(q.toDate, "2026-10-04") == 0 && strcmp(q.toTime, "20:03") == 0);
  q = h.plan(only2 | only7);
  CHECK(q.minRank == 2 && strcmp(q.fromDate, "2026-07-04") == 0);
  CHECK(h.applyResult(q, "2026-08-01T00:00:00", false));
  CHECK(strcmp(h.coveredSince(only2), "2026-07-04T00:00:00") == 0);
  CHECK(strcmp(h.coveredSince(only1), oldest) == 0);  // 1 wasn't searched
  CHECK(strcmp(h.coveredSince(only7), QuakeHistory::kRecordsStart) == 0);
  CHECK(strcmp(h.coveredSince(only1 | only2), oldest) == 0);  // only as complete as its least complete level
  CHECK(h.partialFromRank(only1 | only2) == 2);

  // The database's limit: complete only as far as the last row it gave.
  QuakeHistory c;
  c.setPreloaded("2026-11-01T00:00:00");
  c.liveLoaded(oldest, newest);
  q = c.plan(0);
  CHECK(c.applyResult(q, "2026-09-20T01:00:00", true));
  CHECK(strcmp(c.coveredSince(0), "2026-09-20T01:00:00") == 0);
  q = c.plan(0);
  CHECK(strcmp(q.toDate, "2026-09-20") == 0 && strcmp(q.toTime, "01:00") == 0);

  // Month-ends and January are handled.
  QuakeHistory e;
  e.setPreloaded("2026-11-01T00:00:00");
  e.liveLoaded("2026-03-31T10:00:00", newest);
  CHECK(strcmp(e.plan(0).fromDate, "2026-02-28") == 0);
  QuakeHistory j;
  j.setPreloaded("2026-11-01T00:00:00");
  j.liveLoaded("2026-01-15T10:00:00", newest);
  CHECK(strcmp(j.plan(0).fromDate, "2025-12-15") == 0);

  // Going further each time, until the records run out.
  int guard = 0;
  while (j.canLoadOlder(only2) && guard++ < 600) j.applyResult(j.plan(only2), nullptr, false);
  CHECK(guard < 600 && strncmp(j.coveredSince(only2), "1919-01-01", 10) == 0);

  // Weak rows no longer join up with a live list that moved on, so they must go. Strong ones stay.
  QuakeHistory g;
  g.setPreloaded("2026-11-01T00:00:00");
  g.liveLoaded(oldest, newest);
  g.applyResult(g.plan(0), nullptr, false);
  CHECK(!g.liveLoaded(oldest, newest));
  CHECK(strncmp(g.coveredSince(only1), "2026-09-04", 10) == 0);  // no change for an unchanged list
  CHECK(g.liveLoaded("2026-10-06T00:00:00", newest));
  CHECK(strncmp(g.coveredSince(only1), "2026-10-06", 10) == 0);
  CHECK(strcmp(g.coveredSince(only7), QuakeHistory::kRecordsStart) == 0);
  CHECK(!g.liveLoaded("2026-10-07T00:00:00", newest));  // nothing weak left to drop

  // Later, the live list leaves the snapshot behind: the strong levels fall back to it for now,
  // and a top-up closes the hole, reaching a little past the list's end so it lasts.
  QuakeHistory t;
  t.setPreloaded("2026-11-01T00:00:00");
  t.liveLoaded("2026-11-20T00:00:00", "2026-12-20T00:00:00");
  CHECK(strcmp(t.coveredSince(only7), "2026-11-20T00:00:00") == 0 && t.canLoadOlder(only7) && t.needsTopUp());
  q = t.plan(only7);
  CHECK(q.topUp && q.minRank == 3 && strcmp(q.fromDate, "2026-11-01") == 0 && strcmp(q.toDate, "2026-11-27") == 0 && strcmp(q.toTime, "23:59") == 0);
  CHECK(t.plan(1u << 3).topUp && !t.plan(only1 | only7).topUp);
  CHECK(t.applyResult(q, nullptr, false));
  CHECK(!t.needsTopUp() && !t.canLoadOlder(only7) && strcmp(t.coveredSince(only7), QuakeHistory::kRecordsStart) == 0);
  CHECK(strcmp(t.coveredSince(only1), "2026-11-20T00:00:00") == 0);
  t.liveLoaded("2026-11-25T00:00:00", "2026-12-25T00:00:00");
  CHECK(!t.needsTopUp());  // the margin at work
  t.liveLoaded("2026-11-29T00:00:00", "2026-12-29T00:00:00");
  CHECK(t.needsTopUp() && strcmp(t.plan(only7).fromDate, "2026-11-27") == 0);

  // A long time after flashing: in steps small enough not to hit the row limit.
  QuakeHistory l;
  l.setPreloaded("2026-11-01T00:00:00");
  l.liveLoaded("2028-01-01T00:00:00", "2028-02-01T00:00:00");
  q = l.plan(only7);
  CHECK(strcmp(q.toDate, "2027-01-30") == 0);
  // Should it hit the limit anyway, the rows are no use: what's older than them is missing.
  CHECK(!l.applyResult(q, "2027-01-01T00:00:00", true));
  CHECK(l.needsTopUp() && strcmp(l.plan(only7).fromDate, "2026-11-01") == 0 && strcmp(l.plan(only7).toDate, "2026-11-23") == 0);
  guard = 0;
  while (l.needsTopUp() && guard++ < 100) CHECK(l.applyResult(l.plan(only7), nullptr, false));
  CHECK(guard < 100 && strcmp(l.coveredSince(only7), QuakeHistory::kRecordsStart) == 0);

  // A swarm can fill the live list with only days of rows, too recent for the database. It waits.
  QuakeHistory w;
  w.setPreloaded("2026-11-01T00:00:00");
  w.liveLoaded("2026-12-01T00:00:00", "2026-12-02T00:00:00");
  CHECK(w.needsTopUp() && strcmp(w.plan(only7).toDate, "2026-11-29") == 0);
  w.applyResult(w.plan(only7), nullptr, false);
  w.liveLoaded("2026-12-01T00:00:00", "2026-12-02T00:00:00");
  CHECK(!w.needsTopUp());
  CHECK(strcmp(w.coveredSince(only7), "2026-12-01T00:00:00") == 0);  // and doesn't claim otherwise

  // Merging, in place: time order, no repeats, and the same time needn't be the same earthquake.
  QuakeSummary rows[9] = {dbRow("A3", "2026-09-30T00:00:00"), dbRow("S1", "2026-09-15T00:00:00"), dbRow("S2", "2026-09-15T00:00:00"), dbRow("A1", "2026-09-10T00:00:00")};
  const QuakeSummary batch[] = {dbRow("A3", "2026-09-30T00:00:00"), dbRow("B2", "2026-09-20T00:00:00"), dbRow("S2", "2026-09-15T00:00:00"), dbRow("S3", "2026-09-15T00:00:00"), dbRow("Z", "2026-09-01T00:00:00")};
  int n = quakeMergeOlder(rows, 4, 9, batch, 5);
  CHECK(n == 7);
  const char *const order[] = {"A3", "B2", "A1", "Z"};
  CHECK(strcmp(rows[0].eventId, order[0]) == 0 && strcmp(rows[1].eventId, order[1]) == 0);
  CHECK(strcmp(rows[5].eventId, order[2]) == 0 && strcmp(rows[6].eventId, order[3]) == 0);
  int s15 = 0;
  for (int i = 2; i < 5; i++) s15 += strcmp(rows[i].time, "2026-09-15T00:00:00") == 0;
  CHECK(s15 == 3);
  // A batch that doesn't fit leaves everything alone, rather than dropping the oldest rows.
  CHECK(quakeMergeOlder(rows, 7, 8, batch, 5) == 7 && strcmp(rows[6].eventId, "Z") == 0);
  CHECK(quakeMergeOlder(rows, 7, 8, batch, 0) == 7);
  QuakeSummary none[2];
  CHECK(quakeMergeOlder(none, 0, 2, batch, 2) == 2 && strcmp(none[1].eventId, "B2") == 0);

  // Dropping the weak rows.
  QuakeSummary mixed[] = {dbRow("a", "2026-09-30T00:00:00", 1), dbRow("b", "2026-09-29T00:00:00", 3), dbRow("c", "2026-09-28T00:00:00", 2), dbRow("d", "2026-09-27T00:00:00", 9)};
  CHECK(quakeKeepFromRank(mixed, 4, 3) == 2 && strcmp(mixed[0].eventId, "b") == 0 && strcmp(mixed[1].eventId, "d") == 0);

  // Where the live list's coverage ends in the older rows.
  CHECK(quakeFirstOlderThan(rows, 7, "2026-09-20T00:00:00") == 2);
  CHECK(quakeFirstOlderThan(rows, 7, "2027-01-01T00:00:00") == 0);
  CHECK(quakeFirstOlderThan(rows, 7, "2020-01-01T00:00:00") == 7);
  CHECK(quakeFirstOlderThan(rows, 0, "2026-09-20T00:00:00") == 0);
}

static void testQuakeSnapshot(const std::string &d) {
  CHECK(sizeof(QuakeSnapshotRow) == 20);

  // The database's many ways of writing a time, always ending up sortable.
  char when[20];
  CHECK(quake_detail::dbTime(when, sizeof when, "2026/11/01 07:15:01.5") == kQuakeTimeExact && strcmp(when, "2026-11-01T07:15:01") == 0);
  CHECK(quake_detail::dbTime(when, sizeof when, "1967/10/14 04:50:20") == kQuakeTimeExact && strcmp(when, "1967-10-14T04:50:20") == 0);
  CHECK(quake_detail::dbTime(when, sizeof when, "2000/08/04 04:24") == kQuakeTimeExact && strcmp(when, "2000-08-04T04:24:00") == 0);
  CHECK(quake_detail::dbTime(when, sizeof when, "1935/07/18 03時") == kQuakeTimeHour && strcmp(when, "1935-07-18T03:00:00") == 0);
  CHECK(quake_detail::dbTime(when, sizeof when, "1962年08月") == kQuakeTimeMonth && strcmp(when, "1962-08-01T00:00:00") == 0);
  CHECK(quake_detail::dbTime(when, sizeof when, nullptr) == kQuakeTimeExact && when[0] == 0);
  CHECK(!quake_detail::dbHasCoordinate("0.0000", "0.0000") && quake_detail::dbHasCoordinate("35.6", "139.7"));
  CHECK(!quake_detail::dbHasCoordinate(nullptr, "139.7") && !quake_detail::dbHasCoordinate("", ""));

  // The parser puts what it finds newest first, whatever order the vague ones arrive in.
  const char *const mixed = R"({"res":[{"id":"19620831235530","ot":"1962年08月","name":"日時分不明データ","lat":"0.0000","lon":"0.0000","dep":"不明","mag":"不明","maxI":"震度３"},)"
                            R"({"id":"19620815000000","ot":"1962/08/15 00:00:00.0","name":"x","lat":"35.0","lon":"139.0","dep":"10 km","mag":"5.0","maxI":"震度４"}]})";
  QuakeSummary sorted[2];
  CHECK(quakeParseDbSearch(mixed, strlen(mixed), sorted, 2) == 2);
  CHECK(strcmp(sorted[0].eventId, "19620815000000") == 0 && strcmp(sorted[1].eventId, "19620831235530") == 0);
  CHECK(sorted[1].timePrecision == kQuakeTimeMonth && !sorted[1].hasCoordinate && sorted[1].magnitude[0] == 0 && sorted[1].depthKm == -1);
  CHECK(kQuakeSnapshotCount > 15000);
  CHECK(strncmp(kQuakeSnapshotThrough, "2026-", 5) == 0);
  CHECK(sizeof kQuakeSnapshotNames / sizeof kQuakeSnapshotNames[0] < 65536);

  // In order, strong enough, and every row well formed. Same-second rows must keep distinct ids.
  std::vector<QuakeSummary> all(kQuakeSnapshotCount);
  std::vector<std::string> ids;
  for (int i = 0; i < kQuakeSnapshotCount; i++) {
    quakeFromSnapshot(kQuakeSnapshot[i], kQuakeSnapshotNames[kQuakeSnapshot[i].name], &all[i]);
    CHECK(all[i].maxRank >= QuakeHistory::kPreloadRank && all[i].maxRank <= 9);
    CHECK(strlen(all[i].eventId) == 14 && strlen(all[i].time) == 19);
    CHECK(strncmp(all[i].json, "db:", 3) == 0 && all[i].jsonHasStations);
    if (i > 0) CHECK(strcmp(all[i - 1].time, all[i].time) >= 0);
    ids.push_back(all[i].eventId);
  }
  std::sort(ids.begin(), ids.end());
  CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
  CHECK(strcmp(all[kQuakeSnapshotCount - 1].time, "1920-01-01T00:00:00") < 0);

  // The rows that made the problem visible: an id that isn't the time, and the sevens on record.
  bool kumamoto = false, monthOnly = false, hourOnly = false;
  int sevens = 0;
  for (const auto &q : all) {
    if (strcmp(q.eventId, "20160416040053") == 0) kumamoto = strcmp(q.time, "2016-04-16T04:00:52") == 0;
    if (strcmp(q.eventId, "19620831235530") == 0) monthOnly = strcmp(q.time, "1962-08-01T00:00:00") == 0 && q.timePrecision == kQuakeTimeMonth && !q.hasCoordinate;
    if (strcmp(q.eventId, "19280226223601") == 0) hourOnly = strcmp(q.time, "1928-02-28T01:00:00") == 0 && q.timePrecision == kQuakeTimeHour && q.legacyScale && q.maxRank == 5;
    sevens += q.maxRank == 9;
  }
  CHECK(kumamoto && monthOnly && hourOnly);
  CHECK(sevens >= 8);

  // Merging a batch into all of it finds what's there and adds what's not.
  std::vector<QuakeSummary> store = all;
  store.resize(kQuakeSnapshotCount + 2);
  const QuakeSummary fresh[] = {dbRow("20261102000000", "2026-11-02T00:00:00", 3), all[0]};
  CHECK(quakeMergeOlder(store.data(), kQuakeSnapshotCount, kQuakeSnapshotCount + 2, fresh, 2) == kQuakeSnapshotCount + 1);
  CHECK(strcmp(store[0].eventId, "20261102000000") == 0 && strcmp(store[1].eventId, all[0].eventId) == 0);

  // Unpacked rows must match what the live parser makes of the same earthquakes.
  int compared = 0;
  for (const char *file : {"quake_db_search.json", "quake_db_old.json"}) {
    auto raw = slurp(d + "/" + file);
    std::vector<QuakeSummary> parsed(1000);
    const int n = quakeParseDbSearch(reinterpret_cast<const char *>(raw.data()), raw.size(), parsed.data(), 1000);
    CHECK(n > 0);
    for (int i = 0; i < n; i++) {
      const QuakeSummary &p = parsed[i];
      if (p.maxRank < QuakeHistory::kPreloadRank) continue;
      const QuakeSummary *s = nullptr;
      for (const auto &q : all) {
        if (strcmp(q.eventId, p.eventId) == 0) s = &q;
      }
      CHECK(s != nullptr);
      if (!s) continue;
      compared++;
      CHECK(strcmp(s->json, p.json) == 0 && strcmp(s->name, p.name) == 0 && strcmp(s->time, p.time) == 0);
      CHECK(strcmp(s->magnitude, p.magnitude) == 0 && s->depthKm == p.depthKm);
      CHECK(s->maxRank == p.maxRank && s->legacyScale == p.legacyScale && s->hasCoordinate == p.hasCoordinate && s->timePrecision == p.timePrecision);
      if (p.hasCoordinate) CHECK(fabs(s->lat - p.lat) < 0.006 && fabs(s->lon - p.lon) < 0.006);
    }
  }
  CHECK(compared > 50);
  printf("snapshot: %d rows through %s, %zu KB of rows, %d compared with the parser\n", kQuakeSnapshotCount, kQuakeSnapshotThrough,
         sizeof kQuakeSnapshot / 1024, compared);
}

static void testOlderRow() {
  using B = QuakeBrowser;
  QuakeDriver d;
  d.b.setRowJsonLookup(testRowJson);
  d.b.setRowRankLookup(testRowRank);
  d.b.setAutoReturnMs(0);
  d.b.open(d.now);
  d.b.listLoaded(true, 3, d.now);
  d.b.setOlderEnabled(true);
  CHECK(d.b.olderRowIndex() == 3);
  auto r = B::rowRect(3, 0);
  d.tapAt(300, r.y + 20);
  CHECK(d.b.view() == B::View::kList && d.b.olderLoad() == B::Load::kLoading);
  CHECK(d.b.takeOlderRequest() && !d.b.takeOlderRequest());
  d.tapAt(300, r.y + 20);  // no second request while one is running
  CHECK(!d.b.takeOlderRequest());
  d.b.olderLoaded(true, 8);
  CHECK(d.b.rowCount() == 8 && d.b.visibleCount() == 8 && d.b.olderLoad() == B::Load::kReady);
  CHECK(d.b.olderRowIndex() == 8);
  d.b.olderLoaded(false, 0);  // a failure keeps what's there, and can be retried
  CHECK(d.b.rowCount() == 8 && d.b.olderLoad() == B::Load::kFailed);
  auto rr = B::rowRect(8, d.b.scrollY());
  d.drag(360, 1100, 360, 300);
  rr = B::rowRect(8, d.b.scrollY());
  d.tapAt(300, rr.y + 20);
  CHECK(d.b.takeOlderRequest());
  d.b.olderLoaded(true, 8);

  // When there's nothing older, the row stays as information only.
  d.b.setOlderEnabled(false);
  d.tapAt(300, rr.y + 20);
  CHECK(!d.b.takeOlderRequest() && d.b.view() == B::View::kList);

  // With no matches: row 0 is the message and row 1 the button.
  QuakeDriver e;
  e.b.setRowJsonLookup(testRowJson);
  e.b.setRowRankLookup(testRowRank);
  e.b.setAutoReturnMs(0);
  e.b.open(e.now);
  e.b.listLoaded(true, 8, e.now);
  e.b.setOlderEnabled(true);
  auto f = B::filterRect(8);
  e.tapAt(f.x + 10, f.y + 10);
  CHECK(e.b.visibleCount() == 0 && e.b.olderRowIndex() == 1);
  auto m = B::rowRect(0, 0);
  e.tapAt(300, m.y + 20);
  CHECK(!e.b.takeOlderRequest() && e.b.view() == B::View::kList);
  auto o = B::rowRect(1, 0);
  e.tapAt(300, o.y + 20);
  CHECK(e.b.takeOlderRequest());

  // The divider: above the first row older than the complete point, and only one.
  QuakeDriver v;
  static const auto partialFrom3 = [](int row) { return row >= 3; };
  v.b.setRowJsonLookup(testRowJson);
  v.b.setRowRankLookup(testRowRank);
  v.b.setRowPartialLookup(+partialFrom3);
  v.b.setAutoReturnMs(0);
  v.b.open(v.now);
  v.b.listLoaded(true, 8, v.now);
  CHECK(v.b.visibleCount() == 9 && v.b.visibleRow(2) == 2 && v.b.visibleRow(3) == -1 && v.b.visibleRow(4) == 3 && v.b.visibleRow(8) == 7);
  CHECK(v.b.dividerIndex() == 3);
  // It's the load control, as the row at the very end can be thousands of rows away.
  auto dv = B::rowRect(3, 0);
  v.b.setOlderEnabled(false);
  v.tapAt(300, dv.y + 20);
  CHECK(v.b.view() == B::View::kList && !v.b.takeOlderRequest());
  v.b.setOlderEnabled(true);
  v.tapAt(300, dv.y + 20);
  CHECK(v.b.view() == B::View::kList && v.b.takeOlderRequest() && v.b.olderLoad() == B::Load::kLoading);
  v.b.olderLoaded(true, 8);
  // The rows either side still open the right earthquake.
  auto below = B::rowRect(4, 0);
  v.tapAt(300, below.y + 20);
  CHECK(v.b.view() == B::View::kDetail && v.b.selectedRow() == 3);
  // No row past the point, no divider, and nothing shown counts as nothing.
  QuakeDriver nd;
  static const auto never = [](int) { return false; };
  nd.b.setRowPartialLookup(+never);
  nd.b.open(nd.now);
  nd.b.listLoaded(true, 8, nd.now);
  CHECK(nd.b.visibleCount() == 8 && nd.b.dividerIndex() == -1);
  // Rows beyond 16 bits, which the list now has.
  QuakeDriver big;
  big.b.open(big.now);
  big.b.listLoaded(true, 20000, big.now);
  CHECK(big.b.visibleCount() == 20000 && big.b.visibleRow(19999) == 19999);
  // A load the device started itself shows as loading, and blocks a tap until it's done.
  e.b.olderLoaded(true, 8);
  e.b.olderStarted();
  CHECK(e.b.olderLoad() == B::Load::kLoading);
  e.tapAt(300, o.y + 20);
  CHECK(!e.b.takeOlderRequest());
}

static void testLocationEditor() {
  float v = 0;
  CHECK(parseCoordinate("35.6812", 90, &v) && fabsf(v - 35.6812f) < 0.0001f);
  CHECK(parseCoordinate("-12.5", 90, &v) && v < 0);
  CHECK(parseCoordinate("0", 90, &v) && parseCoordinate("90", 90, &v) && parseCoordinate("-180", 180, &v));
  CHECK(!parseCoordinate("90.01", 90, &v) && !parseCoordinate("181", 180, &v));
  CHECK(!parseCoordinate("", 90, &v) && !parseCoordinate("-", 90, &v) && !parseCoordinate(".", 90, &v));
  CHECK(!parseCoordinate("1.2.3", 90, &v) && !parseCoordinate("1-2", 90, &v));

  struct Driver {
    LocationEditor e;
    uint32_t now = 1000;
    void step(bool p, int x, int y) { e.update(p, x, y, now += 10); }
    void tapAt(int x, int y) { step(true, x, y); step(true, x, y); step(false, 0, 0); }
    void tap(const LocationEditor::Rect &r) { tapAt(r.x + r.w / 2, r.y + r.h / 2); }
    void key(LocationEditor::KeyKind kind, char ch = 0) {
      LocationEditor::Key k[LocationEditor::kKeyCount];
      const int n = LocationEditor::keys(k);
      for (int i = 0; i < n; i++) {
        if (k[i].kind == kind && (kind != LocationEditor::kDigit || k[i].ch == ch)) { tap(k[i].r); return; }
      }
      CHECK(false && "no such key");
    }
    void type(const char *s) {
      for (; *s; s++) {
        if (*s == '.') key(LocationEditor::kPoint);
        else if (*s == '-') key(LocationEditor::kMinus);
        else key(LocationEditor::kDigit, *s);
      }
    }
  };
  LocationEditor::Key k[LocationEditor::kKeyCount];
  const int n = LocationEditor::keys(k);
  CHECK(n == LocationEditor::kKeyCount);
  for (int i = 0; i < n; i++) {
    CHECK(k[i].r.x >= 0 && k[i].r.x + k[i].r.w <= 720 && k[i].r.y + k[i].r.h <= 1280);
    for (int j = 0; j < i; j++) CHECK(k[i].r.x >= k[j].r.x + k[j].r.w || k[j].r.x >= k[i].r.x + k[i].r.w || k[i].r.y >= k[j].r.y + k[j].r.h || k[j].r.y >= k[i].r.y + k[i].r.h);
  }

  float lat = 0, lon = 0;
  Driver d;
  d.e.open(false, 0, 0, d.now);
  CHECK(d.e.visible() && !d.e.canSave() && d.e.active() == 0);
  d.type("35.68.12");  // a second point is ignored
  CHECK(strcmp(d.e.text(0), "35.6812") == 0);
  d.key(LocationEditor::kSave);  // not yet: no longitude
  CHECK(d.e.visible());
  d.tap(LocationEditor::fieldRect(1));
  CHECK(d.e.active() == 1);
  d.type("139.7671");
  CHECK(d.e.canSave());
  d.key(LocationEditor::kSave);
  CHECK(!d.e.visible() && d.e.takeSaved(&lat, &lon) && !d.e.takeSaved(&lat, &lon));
  CHECK(fabsf(lat - 35.6812f) < 0.0001f && fabsf(lon - 139.7671f) < 0.0001f);

  Driver b;  // an existing location shows; an impossible value can't be saved
  b.e.open(true, 35.6812f, 139.7671f, b.now);
  CHECK(strcmp(b.e.text(0), "35.6812") == 0 && b.e.canSave());
  for (int i = 0; i < 7; i++) b.key(LocationEditor::kDelete);
  CHECK(b.e.text(0)[0] == 0 && !b.e.canSave());
  b.type("95");
  CHECK(!b.e.valid(0));
  b.key(LocationEditor::kSave);
  CHECK(b.e.visible() && !b.e.takeSaved(&lat, &lon));
  b.key(LocationEditor::kDelete); b.key(LocationEditor::kDelete);
  b.type("12");
  b.key(LocationEditor::kMinus);  // the sign toggles, at any point
  CHECK(strcmp(b.e.text(0), "-12") == 0);
  b.key(LocationEditor::kMinus);
  CHECK(strcmp(b.e.text(0), "12") == 0);
  b.tapAt(70, 50);  // back keeps nothing
  CHECK(!b.e.visible() && !b.e.takeSaved(&lat, &lon) && !b.e.takeCleared());

  Driver c;  // Clear removes it; left alone it closes without saving
  c.e.open(true, 1, 2, c.now);
  c.key(LocationEditor::kClear);
  CHECK(!c.e.visible() && c.e.takeCleared());
  Driver t;
  t.e.open(true, 1, 2, t.now);
  for (uint32_t ms = 0; ms < LocationEditor::kTimeoutMs + 100; ms += 10) t.step(false, 0, 0);
  CHECK(!t.e.visible() && !t.e.takeSaved(&lat, &lon));

  // The panel row opens it.
  MenuDriver m; m.m.begin(80, 50);
  m.swipe(360, 10, 300);
  m.tap(SettingsMenu::kLocation);
  CHECK(m.m.takeLocationOpenRequest());
  auto r = SettingsMenu::buttonRect(SettingsMenu::kLocation);
  CHECK(r.y + r.h <= SettingsMenu::kPanelH);
}

static void testQuakeDatabase(const std::string &d) {
  static QuakeSummary rows[QuakeHistory::kDbRowLimit];
  const auto search = slurpText(d + "/quake_db_search.json");
  int n = quakeParseDbSearch(search.data(), search.size(), rows, QuakeHistory::kDbRowLimit);
  printf("database search: %d rows in %zu bytes, newest %s %s M%s rank %d\n", n, search.size(), rows[0].time, rows[0].name, rows[0].magnitude, rows[0].maxRank);
  CHECK(n > 0);
  for (int i = 0; i < n; i++) {
    CHECK(rows[i].maxRank >= 5 && rows[i].hasCoordinate && strncmp(rows[i].time, "2011-03", 7) == 0);
    CHECK(strncmp(rows[i].json, "db:", 3) == 0);
    if (i) CHECK(strcmp(rows[i - 1].time, rows[i].time) >= 0);
  }
  // A message instead of rows, as for a date past the database's end.
  const char *message = R"({"res":"現時点の震度データベースにはデータはありません","str":["x"]})";
  CHECK(quakeParseDbSearch(message, strlen(message), rows, 10) == -1);
  CHECK(quakeParseDbSearch("oops", 4, rows, 10) == -1);

  // The earliest records: unknown magnitudes, and the old 5 and 6.
  const auto old = slurpText(d + "/quake_db_old.json");
  n = quakeParseDbSearch(old.data(), old.size(), rows, QuakeHistory::kDbRowLimit);
  int noMag = 0, legacy = 0, unranked = 0;
  for (int i = 0; i < n; i++) {
    noMag += rows[i].magnitude[0] == 0;
    legacy += rows[i].legacyScale;
    unranked += rows[i].maxRank == 0;
  }
  printf("1919-1925: %d rows, %d with no magnitude, %d on the old scale, %d unranked\n", n, noMag, legacy, unranked);
  CHECK(n > 300 && noMag > 0 && legacy > 0 && unranked == 0);

  // The largest report there is, and the map worked out from its stations.
  static QuakeStation stations[4000];
  static QuakeArea areas[256];
  static uint16_t fb[720 * 740];
  QuakeDetail q = {};
  q.stations = stations; q.stationCap = 4000; q.areas = areas; q.areaCap = 256;
  const auto ev = slurpText(d + "/quake_db_event.json");
  CHECK(quakeParseDbEvent(ev.data(), ev.size(), &q));
  printf("2011 report: %s M%s depth %d max %s, %d stations from %zu bytes, first %s\n", q.name, q.magnitude, q.depthKm, quakeIntensityLabel(q.maxRank), q.stationCount, ev.size(), q.areas[0].name);
  CHECK(q.stationCount == 2901 && q.maxRank == 9 && q.hasCoordinate && q.areasAreStations && q.areaCount == 256);
  CHECK(strcmp(q.magnitude, "9.0") == 0 && q.depthKm == 24 && q.areas[0].rank == 9);
  QuakeRegions regions;
  const auto geo = slurpText(d + "/quake_geo.json");
  CHECK(quakeLoadRegions(geo.data(), geo.size(), &regions, malloc));
  quakeRenderMap(regions, q, fb, 720, 740);
  writePpm((d + "/quake_db_event.ppm").c_str(), fb, 720, 740);
  // Region membership: the middle of Honshu is in some region, and the open Pacific in none.
  bool land = false, sea = false;
  for (int i = 0; i < regions.regionCount; i++) {
    land = land || quake_draw::regionContains(regions, regions.regions[i], 138.0f, 36.0f);
    sea = sea || quake_draw::regionContains(regions, regions.regions[i], 150.0f, 30.0f);
  }
  CHECK(land && !sea);
}

static void testSunTimes() {
  // National Astronomical Observatory of Japan, Tokyo, 2026 (eco.mtk.nao.ac.jp/koyomi).
  const struct { int day, rise, set; } almanac[] = {
      {79, 5 * 60 + 44, 17 * 60 + 53},   // 21 Mar
      {171, 4 * 60 + 25, 19 * 60 + 0},   // 21 Jun
      {306, 6 * 60 + 4, 16 * 60 + 44},   // 3 Nov
      {355, 6 * 60 + 47, 16 * 60 + 32},  // 22 Dec
  };
  for (const auto &a : almanac) {
    const SunTimes s = sunTimesJst(35.6581, 139.7414, a.day);
    CHECK(abs(s.rise - a.rise) <= 2);
    CHECK(abs(s.set - a.set) <= 2);
  }
  // The same almanac at the far corners of the country: the location really counts.
  const struct { double lat, lon; int day, rise, set; } places[] = {
      {43.3333, 145.5833, 171, 3 * 60 + 37, 19 * 60 + 2},   // Nemuro, 21 Jun
      {43.3333, 145.5833, 355, 6 * 60 + 47, 15 * 60 + 45},  // Nemuro, 22 Dec
      {31.6, 130.55, 171, 5 * 60 + 13, 19 * 60 + 26},       // Kagoshima, 21 Jun
      {31.6, 130.55, 355, 7 * 60 + 13, 17 * 60 + 19},       // Kagoshima, 22 Dec
  };
  for (const auto &p : places) {
    const SunTimes s = sunTimesJst(p.lat, p.lon, p.day);
    CHECK(abs(s.rise - p.rise) <= 2);
    CHECK(abs(s.set - p.set) <= 2);
  }
  // No sunset or no sunrise at all.
  const SunTimes day = sunTimesJst(80, 139, 171), night = sunTimesJst(80, 139, 355);
  CHECK(day.rise == 0 && day.set == 1439);
  CHECK(night.rise == 720 && night.set == 720);
}

static void testNightSchedule() {
  using D = NightDimmer;
  const SunTimes nov{6 * 60 + 4, 16 * 60 + 44};
  const int high = D::levelOf(80), low = D::levelOf(10);
  CHECK(D::levelOf(100) == 255);

  // Daylight, the lowest period and both of its edges.
  CHECK(D::scheduled(12 * 60, nov, 80) == high);
  CHECK(D::scheduled(nov.rise, nov, 80) == high);
  CHECK(D::scheduled(nov.set, nov, 80) == high);
  CHECK(D::scheduled(22 * 60, nov, 80) == low);
  CHECK(D::scheduled(0, nov, 80) == low);
  CHECK(D::scheduled(4 * 60 + 29, nov, 80) == low);
  CHECK(D::scheduled(4 * 60 + 30, nov, 80) == low);

  // Every minute of the day: never outside the range, and no visible jump anywhere.
  const SunTimes june{4 * 60 + 25, 19 * 60}, polarDay{0, 1439}, polarNight{720, 720};
  const SunTimes faraway{22 * 60, 8 * 60};  // New York's sun on a JST clock
  for (const SunTimes &s : {nov, june, polarDay, polarNight, faraway}) {
    int previous = D::scheduled(1439, s, 100);
    for (int m = 0; m < 1440; m++) {
      const int v = D::scheduled(m, s, 100);
      CHECK(v >= D::levelOf(10) && v <= 255);
      CHECK(abs(v - previous) <= 8);
      previous = v;
    }
  }

  // Monotonic through the evening and the morning.
  for (int m = nov.set; m < 22 * 60; m++) CHECK(D::scheduled(m + 1, nov, 80) <= D::scheduled(m, nov, 80));
  for (int m = 4 * 60 + 30; m < nov.rise; m++) CHECK(D::scheduled(m + 1, nov, 80) >= D::scheduled(m, nov, 80));

  // June's sunrise comes before the lowest period ends: a short ramp, then the set level.
  CHECK(D::scheduled(4 * 60 + 30, june, 80) == low);
  CHECK(D::scheduled(4 * 60 + 45, june, 80) > low && D::scheduled(4 * 60 + 45, june, 80) < high);
  CHECK(D::scheduled(5 * 60, june, 80) == high);

  // A set brightness at the lowest level, the night can't go any lower.
  for (int m = 0; m < 1440; m += 7) CHECK(D::scheduled(m, nov, 10) == D::levelOf(10));
}

static void testNightDimmer() {
  using D = NightDimmer;
  const SunTimes sun{6 * 60, 17 * 60};
  const int night = 23 * 60, high = D::levelOf(80), low = D::levelOf(10);

  // Clock unknown: the set brightness, day or night.
  D d;
  CHECK(d.update(1000, false, false, -1, sun, 80) == high);

  // The clock arriving at night fades down, rather than snapping.
  CHECK(d.update(1005, false, false, night, sun, 80) < high);
  uint32_t t = 1005;
  while (t < 4000) d.update(t += 5, false, false, night, sun, 80);
  CHECK(d.update(t += 5, false, false, night, sun, 80) == low);

  // A touch is immediate and lasts while the finger is down and for the hold after it.
  CHECK(d.update(t += 5, true, false, night, sun, 80) == high);
  const uint32_t released = t;
  CHECK(d.update(t += 5, false, false, night, sun, 80) == high);
  CHECK(d.update(released + D::kTouchHoldMs - 1, false, false, night, sun, 80) == high);
  t = released + D::kTouchHoldMs + 100;
  const int fading = d.update(t, false, false, night, sun, 80);
  CHECK(fading < high && fading > low);
  while (t < released + D::kTouchHoldMs + 3000) d.update(t += 5, false, false, night, sun, 80);
  CHECK(d.update(t += 5, false, false, night, sun, 80) == low);

  // Changing the set brightness while held shows at once, in both directions.
  d.update(t += 5, true, false, night, sun, 80);
  CHECK(d.update(t += 5, false, false, night, sun, 70) == D::levelOf(70));
  CHECK(d.update(t += 5, false, false, night, sun, 90) == D::levelOf(90));
  t += D::kTouchHoldMs + 5000;
  d.update(t, false, false, night, sun, 80);
  CHECK(d.update(t += 5, false, false, night, sun, 80) == low);

  // An alert is full at once, whatever the hour or the set brightness, then fades back.
  CHECK(d.update(t += 5, false, true, night, sun, 30) == 255);
  CHECK(d.update(t += 60000, false, true, night, sun, 30) == 255);
  // A touch during an alert doesn't take it below full.
  CHECK(d.update(t += 5, true, true, night, sun, 30) == 255);
  t += D::kTouchHoldMs + 5000;
  CHECK(d.update(t, false, false, night, sun, 30) < 255);
  t += 5000;
  d.update(t, false, false, night, sun, 30);
  CHECK(d.update(t += 5, false, false, night, sun, 30) == low);

  // Daytime holds nothing back, and millis() wrapping doesn't confuse the hold.
  D w;
  uint32_t near = 0xFFFFFFFFu - 1000;
  w.update(near, true, false, night, sun, 80);
  CHECK(w.update(near + 2000, false, false, night, sun, 80) == high);
}

static void testAutoReturnSetting() {
  using M = SettingsMenu;
  MenuDriver d; d.m.begin(80, 50);
  CHECK(d.m.autoReturnSeconds() == 10);  // the default
  d.swipe(360, 10, 300);
  d.tap(M::kAutoPlus);
  CHECK(d.m.autoReturnSeconds() == 15);
  for (int i = 0; i < 12; i++) d.tap(M::kAutoPlus);
  CHECK(d.m.autoReturnSeconds() == 120 && !d.m.canApply(M::kAutoPlus));
  for (int i = 0; i < 12; i++) d.tap(M::kAutoMinus);
  CHECK(d.m.autoReturnSeconds() == 0 && !d.m.canApply(M::kAutoMinus));  // Off
  CHECK(d.m.brightness() == 80 && d.m.sound() == 50);
  d.idle(M::kAutoCloseMs + 1000);
  CHECK(d.m.takeSaveRequest());  // it counts as a change to save
  MenuDriver e; e.m.begin(80, 50, 30);
  CHECK(e.m.autoReturnSeconds() == 30);
  MenuDriver o; o.m.begin(80, 50, 0);
  CHECK(o.m.autoReturnSeconds() == 0);
  MenuDriver bad; bad.m.begin(80, 50, 7);  // not a stop: back to the default
  CHECK(bad.m.autoReturnSeconds() == 10);
  for (auto b : {M::kAutoMinus, M::kAutoPlus, M::kWifi}) {
    auto r = M::buttonRect(b);
    CHECK(r.y + r.h <= M::kPanelH);
  }
}

static void testQuakeAgo() {
  const int64_t base = (int64_t)QuakeHistory::dayNumber("2026-11-04") * 86400 + 12 * 3600;  // noon
  const auto ago = [&](const char *t) {
    static char out[32];
    quakeAgoText(t, base, out, sizeof out);
    return std::string(out);
  };
  CHECK(ago("2026-11-04T12:00:00") == "just now");
  CHECK(ago("2026-11-04T11:59:01") == "just now");
  CHECK(ago("2026-11-04T11:59:00") == "1 minute ago");
  CHECK(ago("2026-11-04T12:30:00") == "just now");  // the clock a little behind
  CHECK(ago("2026-11-04T11:26:00") == "34 minutes ago");
  CHECK(ago("2026-11-04T11:00:01") == "59 minutes ago");
  CHECK(ago("2026-11-04T11:00:00") == "1 hour ago");
  CHECK(ago("2026-11-03T12:00:01") == "23 hours ago");
  CHECK(ago("2026-11-03T12:00:00") == "1 day ago");
  CHECK(ago("2026-11-01T12:00:00") == "3 days ago");
  CHECK(ago("2026-10-06T12:00:00") == "29 days ago");
  CHECK(ago("2026-10-05T12:00:00") == "1 month ago");
  CHECK(ago("2026-05-04T12:00:00") == "6 months ago");
  CHECK(ago("2025-11-05T12:00:00") == "11 months ago");
  CHECK(ago("2025-11-04T12:00:00") == "1 year ago");
  CHECK(ago("2024-01-01T16:10:00") == "2 years ago");
  CHECK(ago("1923-09-01T11:58:00") == "103 years ago");
  CHECK(ago("1919-01-01T00:00") == "107 years ago");  // no seconds
  CHECK(ago("1962-08") == "");  // can't be read
  CHECK(ago("") == "");
}

static void testListWaiting() {
  using B = QuakeBrowser;
  QuakeDriver d;
  d.b.setRowJsonLookup(testRowJson);
  // The first visit waits for the list, with the rows out of reach.
  d.b.olderLoaded(true, 50);
  d.b.open(d.now);
  CHECK(d.b.listWaiting() && d.b.takeListRequest());
  d.b.takeDirty();
  d.idle(B::kSpinMs + 50);
  CHECK(d.b.takeDirty());  // the spinner turns
  d.drag(360, 900, 360, 500);
  CHECK(d.b.scrollY() == 0);
  d.drag(360, 400, 360, 400);
  CHECK(d.b.view() == B::View::kList);
  d.b.listLoaded(true, 60, d.now);
  CHECK(!d.b.listWaiting() && d.b.rowCount() == 60);
  d.drag(360, 900, 360, 500);
  CHECK(d.b.scrollY() == 400);
  // Coming back soon shows the rows at once, even with a refresh due.
  d.b.close();
  d.idle(B::kRefreshMs + 1000);
  d.b.open(d.now);
  CHECK(d.b.takeListRequest() && !d.b.listWaiting());
  d.b.listLoaded(true, 60, d.now);
  // After a long time, it waits again.
  d.b.close();
  d.now += B::kStaleListMs;
  d.b.open(d.now);
  CHECK(d.b.takeListRequest() && d.b.listWaiting());
  // A back swipe still works while waiting.
  d.drag(100, 600, 400, 600);
  int fingerX;
  CHECK(d.b.takeBackDrag(&fingerX) && fingerX == 100);
  // A failure shows the rows, and being offline adds no wait to later visits.
  d.b.listLoaded(false, 0, d.now);
  CHECK(!d.b.listWaiting() && d.b.rowCount() == 60 && d.b.listRefreshFailed());
  d.b.close();
  d.now += B::kStaleListMs;
  d.b.open(d.now);
  CHECK(d.b.takeListRequest() && !d.b.listWaiting());
  d.b.listLoaded(true, 60, d.now);
  // A network that never answers can't hide them for long.
  d.b.close();
  d.now += B::kStaleListMs;
  d.b.open(d.now);
  CHECK(d.b.listWaiting());
  d.idle(B::kListWaitMaxMs + 100);
  CHECK(!d.b.listWaiting());
  // Reopening while the very first answer is still on its way keeps waiting.
  QuakeDriver e;
  e.b.olderLoaded(true, 50);
  e.b.open(e.now);
  e.b.takeListRequest();
  e.b.close();
  e.b.open(e.now);
  CHECK(e.b.listWaiting() && !e.b.takeListRequest());
}

static void testNightModeTimes() {
  NightSchedule n;
  CHECK(!n.screenOff(23 * 60) && !n.screenOff(-1));  // off by default
  n.mode = NightSchedule::kScheduled;
  n.start = 22 * 60;
  n.end = 6 * 60 + 30;
  CHECK(n.screenOff(22 * 60) && n.screenOff(0) && n.screenOff(6 * 60 + 29));
  CHECK(!n.screenOff(6 * 60 + 30) && !n.screenOff(21 * 60 + 59) && !n.screenOff(12 * 60));
  CHECK(!n.screenOff(-1));  // no clock: stays on
  n.start = 9 * 60;
  n.end = 17 * 60;  // the same day
  CHECK(n.screenOff(9 * 60) && !n.screenOff(17 * 60) && !n.screenOff(8 * 60) && !n.screenOff(23 * 60));
  n.end = n.start;
  CHECK(!n.screenOff(9 * 60));
  n.mode = NightSchedule::kContinuous;
  CHECK(n.screenOff(12 * 60) && n.screenOff(-1));
  CHECK(NightSchedule().wakeSeconds == 30);
  for (int w : {0, 3, 65, -5}) {
    NightSchedule b;
    b.wakeSeconds = w;
    b.normalize();
    CHECK(b.wakeSeconds == 30);
  }
  NightSchedule kept;
  kept.wakeSeconds = 45;
  kept.normalize();
  CHECK(kept.wakeSeconds == 45);
  NightSchedule bad;
  bad.mode = 7;
  bad.start = -5;
  bad.end = 1440;
  bad.normalize();
  CHECK(bad.mode == NightSchedule::kOff && bad.start == 22 * 60 && bad.end == 270);
}

static void testNightDimmerOff() {
  using D = NightDimmer;
  const SunTimes sun{6 * 60, 17 * 60};
  const int noon = 12 * 60, high = D::levelOf(80);
  uint32_t t = 1000;
  D d;
  CHECK(d.update(t, false, false, noon, sun, 80, false) == high);
  // Fades out, whatever the hour.
  CHECK(d.update(t += 100, false, false, noon, sun, 80, true) > 0);
  for (int i = 0; i < 500; i++) d.update(t += 5, false, false, noon, sun, 80, true);
  CHECK(d.update(t += 5, false, false, noon, sun, 80, true) == 0);
  // The first touch wakes it at once, and the pages don't see that touch.
  CHECK(d.update(t += 5, true, false, noon, sun, 80, true) == high);
  CHECK(d.wakeTouch());
  d.update(t += 5, true, false, noon, sun, 80, true);
  CHECK(d.wakeTouch());  // for as long as the finger stays down
  CHECK(d.update(t += 5, false, false, noon, sun, 80, true) == high);
  CHECK(!d.wakeTouch());
  // The next touch is an ordinary one, and extends the hold.
  d.update(t += 5, true, false, noon, sun, 80, true);
  CHECK(!d.wakeTouch());
  d.update(t += 5, false, false, noon, sun, 80, true);
  t += D::kTouchHoldMs - 100;
  CHECK(d.update(t, false, false, noon, sun, 80, true) == high);
  t += 200;
  for (int i = 0; i < 500; i++) d.update(t += 5, false, false, noon, sun, 80, true);
  CHECK(d.update(t += 5, false, false, noon, sun, 80, true) == 0);
  // An alert wakes it to full, and the screen goes off again afterwards.
  CHECK(d.update(t += 5, false, true, noon, sun, 80, true) == 255);
  for (int i = 0; i < 500; i++) d.update(t += 5, false, false, noon, sun, 80, true);
  CHECK(d.update(t += 5, false, false, noon, sun, 80, true) == 0);
  // Touching during an alert isn't swallowed: the screen was already lit.
  CHECK(d.update(t += 5, false, true, noon, sun, 80, true) == 255);
  d.update(t += 5, true, true, noon, sun, 80, true);
  CHECK(!d.wakeTouch());
  // Leaving the off period returns to the day's level at once.
  D e;
  uint32_t u = 1000;
  e.update(u, false, false, noon, sun, 80, true);
  CHECK(e.update(u += 5, false, false, noon, sun, 80, true) == 0);
  CHECK(e.update(u += 5, false, false, noon, sun, 80, false) == high);
  // The hold can be changed, for dimming as well as for the screen being off.
  D h;
  h.setHoldMs(5000);
  uint32_t v = 1000;
  h.update(v, false, false, noon, sun, 80, true);
  CHECK(h.update(v += 5, true, false, noon, sun, 80, true) == high);
  h.update(v += 5, false, false, noon, sun, 80, true);
  CHECK(h.update(v += 4900, false, false, noon, sun, 80, true) == high);
  v += 200;
  for (int i = 0; i < 500; i++) h.update(v += 5, false, false, noon, sun, 80, true);
  CHECK(h.update(v += 5, false, false, noon, sun, 80, true) == 0);
  // Started by hand, the screen shows first.
  D f;
  f.wake(1000);
  CHECK(f.update(1005, false, false, -1, sun, 80, true) == high);
  CHECK(!f.wakeTouch());
  // A restart at night goes dark straight away, with no fade.
  D g;
  CHECK(g.update(1000, false, false, noon, sun, 80, true) == 0);
}

struct NightDriver {
  NightModeEditor e;
  uint32_t t = 1000;
  void at(int x, int y) {
    e.update(true, x, y, t += 20);
    e.update(false, x, y, t += 20);
  }
  void mode(int m) {
    auto r = NightModeEditor::modeRect(m);
    at(r.x + 5, r.y + 5);
  }
  void field(int f) {
    auto r = NightModeEditor::fieldRect(f);
    at(r.x + 5, r.y + 5);
  }
  void wake(bool plus) {
    auto r = NightModeEditor::wakeRect(plus);
    at(r.x + 5, r.y + 5);
  }
  void key(char ch) {  // 'D' for Del, 'S' for Save
    NightModeEditor::Key k[NightModeEditor::kKeyCount];
    const int n = NightModeEditor::keys(k);
    for (int i = 0; i < n; i++) {
      const bool hit = ch == 'D' ? k[i].kind == NightModeEditor::kDelete : ch == 'S' ? k[i].kind == NightModeEditor::kSave : k[i].ch == ch;
      if (hit) at(k[i].r.x + 5, k[i].r.y + 5);
    }
  }
  void type(const char *s) {
    for (; *s; s++) key(*s);
  }
  std::string shown(int f) {
    char t[6];
    e.display(f, t);
    return t;
  }
};

static void testNightEditor() {
  using E = NightModeEditor;
  NightSchedule now;
  {  // Everything fits on the screen.
    E::Key k[E::kKeyCount];
    CHECK(E::keys(k) == E::kKeyCount);
    for (auto &x : k) CHECK(x.r.y + x.r.h <= 1280 && x.r.x + x.r.w <= 720 && x.r.y >= E::fieldRect(0).y + E::fieldRect(0).h);
    CHECK(E::modeRect(2).x + E::modeRect(2).w <= 720);
  }
  {  // Typing a schedule that spans midnight, moving on by itself.
    NightDriver d;
    d.e.open(now, d.t);
    CHECK(d.shown(0) == "22:00" && d.shown(1) == "04:30" && d.e.mode() == NightSchedule::kOff);
    d.mode(NightSchedule::kScheduled);
    d.type("2315");
    CHECK(d.shown(0) == "23:15" && d.e.active() == 1);
    d.type("06");
    CHECK(d.shown(1) == "06:__" && !d.e.canSave());
    d.key('S');
    CHECK(d.e.visible());  // Save does nothing yet
    d.type("45");
    CHECK(d.shown(1) == "06:45" && d.e.canSave());
    d.type("9");  // the field is full
    CHECK(d.shown(1) == "06:45");
    d.key('S');
    CHECK(!d.e.visible());
    NightSchedule out;
    CHECK(d.e.takeSaved(&out));
    CHECK(out.mode == NightSchedule::kScheduled && out.start == 23 * 60 + 15 && out.end == 6 * 60 + 45);
    CHECK(!d.e.takeSaved(&out));
  }
  {  // Only digits that can make a time.
    NightDriver d;
    d.e.open(now, d.t);
    d.mode(NightSchedule::kScheduled);
    d.type("3");
    CHECK(d.shown(0) == "22:00");  // refused, and nothing replaced
    d.type("24");
    CHECK(d.shown(0) == "2_:__");
    d.type("3");
    CHECK(d.shown(0) == "23:__");
    d.type("6");
    CHECK(d.shown(0) == "23:__");
    d.type("59");
    CHECK(d.shown(0) == "23:59" && d.e.active() == 1);
    d.type("19");
    CHECK(d.shown(1) == "19:__");
    d.key('D');
    CHECK(d.shown(1) == "1_:__");
    d.key('D');
    d.key('D');
    CHECK(d.shown(1) == "__:__" && !d.e.canSave());
  }
  {  // Tapping a field starts it over. The same time twice can't be saved.
    NightDriver d;
    d.e.open(now, d.t);
    d.mode(NightSchedule::kScheduled);
    d.field(1);
    d.type("2200");
    CHECK(d.e.timesEqual() && !d.e.canSave());
    d.field(0);
    CHECK(d.e.active() == 0);
    d.type("2130");
    CHECK(d.e.canSave());
  }
  {  // Continuous and Off keep the times, and ignore the keys.
    NightDriver d;
    d.e.open(now, d.t);
    d.mode(NightSchedule::kContinuous);
    d.type("1200");
    CHECK(d.shown(0) == "22:00");
    d.field(1);
    CHECK(d.e.active() == 0);
    CHECK(d.e.canSave());
    d.key('S');
    NightSchedule out;
    CHECK(d.e.takeSaved(&out));
    CHECK(out.mode == NightSchedule::kContinuous && out.start == 22 * 60 && out.end == 270);
  }
  {  // A half-typed time left behind by switching mode doesn't get saved.
    NightDriver d;
    d.e.open(now, d.t);
    d.mode(NightSchedule::kScheduled);
    d.type("1");
    d.mode(NightSchedule::kContinuous);
    CHECK(d.e.canSave());
    d.key('S');
    NightSchedule out;
    CHECK(d.e.takeSaved(&out));
    CHECK(out.mode == NightSchedule::kContinuous && out.start == 22 * 60);
  }
  {  // The time after a touch steps by 5 s, in any mode, and stops at each end.
    NightDriver d;
    d.e.open(now, d.t);
    CHECK(d.e.wakeSeconds() == 30);
    d.wake(true);
    CHECK(d.e.wakeSeconds() == 35);
    for (int i = 0; i < 20; i++) d.wake(true);
    CHECK(d.e.wakeSeconds() == 60 && !d.e.canWake(true) && d.e.canWake(false));
    for (int i = 0; i < 20; i++) d.wake(false);
    CHECK(d.e.wakeSeconds() == 5 && !d.e.canWake(false) && d.e.canWake(true));
    d.wake(true);
    d.key('S');
    NightSchedule out;
    CHECK(d.e.takeSaved(&out));
    CHECK(out.wakeSeconds == 10 && out.mode == NightSchedule::kOff);
    // Back leaves it as it was, and it opens with the saved value.
    NightSchedule set = out;
    d.e.open(set, d.t);
    CHECK(d.e.wakeSeconds() == 10);
    d.wake(true);
    d.at(20, 20);
    CHECK(!d.e.takeSaved(&out));
    // The rows don't overlap.
    const auto m = E::modeRect(0), f = E::fieldRect(0), w = E::wakeRect(false);
    E::Key k[E::kKeyCount];
    E::keys(k);
    CHECK(m.y + m.h < f.y && f.y + f.h < w.y && w.y + w.h < k[0].r.y);
  }
  {  // Back discards, and so does waiting.
    NightDriver d;
    d.e.open(now, d.t);
    d.mode(NightSchedule::kContinuous);
    d.at(20, 20);
    CHECK(!d.e.visible());
    NightSchedule out;
    CHECK(!d.e.takeSaved(&out));
    d.e.open(now, d.t);
    d.e.update(false, 0, 0, d.t + E::kTimeoutMs + 1);
    CHECK(!d.e.visible());
  }
  // The panel row opens it.
  MenuDriver m; m.m.begin(80, 50);
  m.swipe(360, 10, 300);
  m.tap(SettingsMenu::kNight);
  CHECK(m.m.takeNightOpenRequest());
  auto r = SettingsMenu::buttonRect(SettingsMenu::kNight);
  CHECK(r.y + r.h <= SettingsMenu::kPanelH - 16);  // clear of the handle
}

// argv[1]=fixture dir
int main(int argc, char **argv) {
  const std::string d = argv[1];
  compareWithBmp(d + "/base.gif", 352, 400, d + "/base.bmp");
  compareWithBmp(d + "/scale.gif", 55, 301, d + "/scale.bmp");
  compareWithBmp(d + "/stations.gif", 352, 400, d + "/stations.bmp");
  compareWithBmp(d + "/est.gif", 352, 400, d + "/est.bmp");
  compareWithBmp(d + "/ps.gif", 352, 400, d + "/ps.bmp");
  testTruncated(d + "/stations.gif", 352, 400);
  testAxis();
  testMix();
  testMenu();
  testMenuWifiButton();
  testAutoReturnSetting();
  testQuakeHelpers();
  testQuakeListMerging();
  testQuakeViewFallbacks();
  testEdgeSwipe();
  testPageSlide();
  testQuakeBrowser();
  testQuakeFilter();
  testRowHighlightDelay();
  testBatteryGauge();
  testSunTimes();
  testNightSchedule();
  testNightDimmer();
  testQuakeAgo();
  testListWaiting();
  testNightModeTimes();
  testNightDimmerOff();
  testNightEditor();
  testRankAndDistance();
  testQuakeHistory();
  testOlderRow();
  testQuakeSnapshot(d);
  testLocationEditor();
  testQuakeRealData(d);
  testQuakeDatabase(d);
  testPasswordRules();
  testKeyboardCharacters();
  testWifiTyping();
  testWifiList();
  testKnownNetworks();
  testSavedNetworkRows();

  // Full composite in the page's layer order, with the firmware's geometry.
  std::vector<uint32_t> map(352 * 400, 0xFFFFFFFF);
  std::vector<uint8_t> work(352 * 400);
  for (auto n : {"base", "est", "stations", "ps"}) {
    auto g = slurp(d + "/" + n + ".gif");
    CHECK(gifDrawOver(g.data(), g.size(), map.data(), 352, 400, work.data()));
  }
  std::vector<ResampleAxis> ax(720), ay(818);
  resampleAxis(ax.data(), 352, 720);
  resampleAxis(ay.data(), 400, 818);
  std::vector<uint32_t> big(720 * 818);
  resampleRgba(map.data(), 352, ax.data(), 720, ay.data(), 818, big.data());
  std::vector<uint16_t> fb(720 * 840, 0xFFFF);
  blitOverSwap565(fb.data(), 720, 840, 0, 0, big.data(), 720, 818);
  std::vector<uint32_t> scale(55 * 301, 0);
  auto g = slurp(d + "/scale.gif");
  std::vector<uint8_t> w2(55 * 301);
  CHECK(gifDrawOver(g.data(), g.size(), scale.data(), 55, 301, w2.data()));
  std::vector<ResampleAxis> sx(96), sy(616);
  resampleAxis(sx.data(), 55, 96);
  resampleAxis(sy.data(), 301, 616);
  std::vector<uint32_t> sbig(96 * 616);
  resampleRgba(scale.data(), 55, sx.data(), 96, sy.data(), 616, sbig.data());
  blitOverSwap565(fb.data(), 720, 840, 624, 207, sbig.data(), 96, 616);
  writePpm((d + "/composite.ppm").c_str(), fb.data(), 720, 840);

  printf(failures ? "%d FAILURES\n" : "ALL PASS\n", failures);
  return failures ? 1 : 0;
}
