// Networks that have connected before, most recent first. Stored as one blob.
// Host-testable: no Arduino dependencies.
#pragma once
#include <stdio.h>
#include <string.h>

struct KnownNetwork {
  char ssid[33];
  char pass[65];
};

class KnownNetworks {
 public:
  static constexpr int kMax = 8;

  const KnownNetwork *find(const char *ssid) const {
    for (int i = 0; i < count(); i++) {
      if (strcmp(items_[i].ssid, ssid) == 0) return &items_[i];
    }
    return nullptr;
  }

  int count() const {
    int n = 0;
    while (n < kMax && items_[n].ssid[0]) n++;
    return n;
  }

  // Moves it to the front, or adds it there, dropping the oldest when full.
  void remember(const char *ssid, const char *pass) {
    int at = 0;
    const int n = count();
    while (at < n && strcmp(items_[at].ssid, ssid) != 0) at++;
    if (at == kMax) at = kMax - 1;
    KnownNetwork fresh = {};
    snprintf(fresh.ssid, sizeof fresh.ssid, "%s", ssid);
    snprintf(fresh.pass, sizeof fresh.pass, "%s", pass);
    memmove(&items_[1], &items_[0], sizeof(KnownNetwork) * at);
    items_[0] = fresh;
  }

  // Adds at the end if absent and there's room: for defaults that shouldn't push anything out.
  void seed(const char *ssid, const char *pass) {
    if (find(ssid)) return;
    const int n = count();
    if (n == kMax) return;
    snprintf(items_[n].ssid, sizeof items_[n].ssid, "%s", ssid);
    snprintf(items_[n].pass, sizeof items_[n].pass, "%s", pass);
  }

  void *data() { return items_; }
  const void *data() const { return items_; }
  static constexpr size_t byteSize() { return sizeof(KnownNetwork) * kMax; }

  // After loading from storage: never trust that strings end.
  void normalize() {
    for (auto &k : items_) {
      k.ssid[sizeof k.ssid - 1] = 0;
      k.pass[sizeof k.pass - 1] = 0;
    }
  }

 private:
  KnownNetwork items_[kMax] = {};
};
