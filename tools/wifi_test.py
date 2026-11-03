#!/usr/bin/env python3
"""Drives the Wi-Fi selector on the Tab5 through injected touches.

usage: wifi_test.py PORT OUTDIR
Only ever connects to the network it's already on. Keyboard screens are reached through another
secured network in range, but nothing typed there is ever submitted. Drops the connection
briefly to test reconnecting. Saves screenshots to OUTDIR.
"""
import os, select, subprocess, sys, time

sys.path.insert(0, os.path.dirname(__file__))
from menu_test import Tab5, MINUS_X, PLUS_X, SOUND_Y, WIFI_Y

HIDDEN, LIST, PASSWORD, CONNECTING = "0", "1", "2", "3"
KIND_CHAR, KIND_SHIFT, KIND_DEL, KIND_PAGE, KIND_SPACE, KIND_CONNECT = range(6)
BACK = (70, 50)
SHOW = (635, 160)


class Driver(Tab5):
    def lines(self, cmd, prefix):
        while select.select([self.fd], [], [], 0)[0]:
            os.read(self.fd, 65536)
        self.send(cmd, 0.0)
        buf, end = b"", time.time() + 3
        while time.time() < end and f"{prefix} end".encode() not in buf:
            if select.select([self.fd], [], [], 0.1)[0]:
                buf += os.read(self.fd, 65536)
        return [l.decode("utf-8", "replace").strip() for l in buf.split(b"\n") if l.startswith(prefix.encode() + b" ")]

    def keys(self):
        out = []
        for l in self.lines("K", "key"):
            p = l.split()
            if p[1] != "end":
                out.append(tuple(int(v) for v in p[1:]))
        return out

    def tap_key(self, match):
        for kind, ch, x, y, w, h in self.keys():
            if match(kind, ch):
                self.tap(x + w // 2, y + h // 2)
                return True
        return False

    def tap_kind(self, kind):
        assert self.tap_key(lambda k, c: k == kind), kind

    def type_char(self, c):
        o = ord(c)
        for _ in range(4):
            if self.tap_key(lambda k, ch: k in (KIND_CHAR, KIND_SPACE) and ch == o):
                return
            page = self.status()["page"]
            if page == "2" or not c.isalpha():
                self.tap_kind(KIND_PAGE)
            else:
                self.tap_kind(KIND_SHIFT)
        sys.exit("could not type a character")

    def type_text(self, text):
        for c in text:
            self.type_char(c)
        # Leave the keyboard on plain letters.
        if self.status()["page"] == "2":
            self.tap_kind(KIND_PAGE)

    def wait(self, pred, seconds):
        end = time.time() + seconds
        while time.time() < end:
            s = self.status()
            if pred(s):
                return s
            time.sleep(0.5)
        return self.status()

    def open_selector(self):
        self.swipe(360, 10, 420)
        self.tap(360, WIFI_Y)

    def row_of(self, ssid):
        for l in self.lines("W", "net"):
            p = l.split(" ", 5)
            if len(p) == 6 and p[5] == ssid:
                return int(p[1])
        return None

    def first_unsaved_secured_row(self):
        for l in self.lines("W", "net"):
            p = l.split(" ", 5)
            if len(p) == 6 and p[3] == "1" and p[4] == "0":
                return int(p[1])
        return None

    def tap_row(self, row):
        if row >= 11:
            sys.exit("network too far down the list to tap in this test")
        self.tap(360, 110 + row * 100 + 50)


def main():
    port, out = sys.argv[1], sys.argv[2]
    t = Driver(port)
    ssid = t.status()["ssid"]
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        print(("ok   " if ok else "FAIL ") + name, detail)
        failures += 0 if ok else 1

    def shot(name):
        subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "screenshot.py"), port, f"{out}/{name}.ppm"],
                       check=True, capture_output=True)
        subprocess.run(["sips", "-s", "format", "png", f"{out}/{name}.ppm", "--out", f"{out}/{name}.png"], capture_output=True)

    def open_and_scan():
        t.open_selector()
        s = t.wait(lambda s: s["view"] == LIST and s["scanning"] == "0", 15)
        return s

    s0 = t.status()
    print("start:", {k: s0[k] for k in ("uptime", "wifi", "ssid", "wifisrc", "known", "heap")})

    # List
    t.swipe(360, 10, 420)
    shot("menu")
    t.tap(360, WIFI_Y)
    check("Wi-Fi row opens the selector", t.status()["view"] == LIST)
    s = t.wait(lambda s: s["scanning"] == "0", 15)
    check("scan finishes", s["scanning"] == "0" and int(s["nets"]) > 0, "nets=" + s["nets"])
    row = t.row_of(ssid)
    check("current network is listed", row is not None, f"row={row}")
    shot("list")
    t.tap(*BACK)
    check("back returns to the map", t.status()["view"] == HIDDEN)
    n0 = int(t.status()["shown"]); time.sleep(3)
    check("map resumes", int(t.status()["shown"]) >= n0 + 2)

    # The connected network: back to the map, with no reconnect.
    open_and_scan()
    t.tap_row(t.row_of(ssid))
    s = t.status()
    check("tapping the connected network just closes", s["view"] == HIDDEN and s["wifi"] == "3")

    # Keyboard, through a network with no saved password. Never submitted.
    open_and_scan()
    row = t.first_unsaved_secured_row()
    if row is None:
        print("skip keyboard: no unsaved secured network in range")
    else:
        t.tap_row(row)
        check("an unsaved network opens the keyboard", t.status()["view"] == PASSWORD)
        shot("keyboard")
        t.type_text("abcdefg")
        check("typing registers", t.status()["pwlen"] == "7")
        t.tap_kind(KIND_CONNECT)
        check("7 characters can't connect", t.status()["view"] == PASSWORD)
        t.tap_kind(KIND_DEL)
        check("delete removes one", t.status()["pwlen"] == "6")
        t.tap_kind(KIND_PAGE)
        shot("symbols")
        t.tap_kind(KIND_PAGE)
        t.type_text("gX#1 z")  # includes a capital, a symbol, a digit and a space
        check("mixed characters type", t.status()["pwlen"] == "12")
        shot("masked")
        t.tap(*SHOW)
        shot("shown")
        t.tap(*SHOW)
        t.tap(*BACK)
        check("back from the keyboard forgets the text", t.status()["pwlen"] == "0")
    t.tap(*BACK)

    # Scanning must also work with no connection, as after moving somewhere new. A saved
    # network must then rejoin with one tap and no password.
    t.send("D", 0.3)
    t.wait(lambda s: s["wifi"] != "3", 5)
    s = open_and_scan()
    check("scan works while disconnected", s["scanning"] == "0" and int(s["nets"]) > 0, "nets=" + s["nets"])
    shot("disconnected")
    t.tap_row(t.row_of(ssid))
    s = t.status()
    check("a saved network skips the keyboard", s["view"] == CONNECTING, "view=" + s["view"])
    s = t.wait(lambda s: s["view"] != CONNECTING, 30)
    check("and connects", s["view"] == HIDDEN and s["wifi"] == "3" and s["ssid"] == ssid, f'view={s["view"]} wifi={s["wifi"]}')
    n0 = int(t.status()["shown"])
    s = t.wait(lambda s: int(s["shown"]) >= n0 + 3, 20)
    check("frames resume after connecting", int(s["shown"]) >= n0 + 3)

    # Reconnects by itself if the selector was only looked at.
    t.send("D", 0.3)
    t.wait(lambda s: s["wifi"] != "3", 5)
    open_and_scan()
    t.tap(*BACK)
    s = t.wait(lambda s: s["wifi"] == "3", 50)
    check("reconnects after the selector closes", s["wifi"] == "3")

    # An alert closes the selector. Muted first, so this test is silent.
    t.swipe(360, 10, 420)
    sound0 = int(t.status()["sound"])
    while int(t.status()["sound"]) > 0:
        t.tap(MINUS_X, SOUND_Y)
    t.tap(360, WIFI_Y)
    t.wait(lambda s: s["scanning"] == "0", 15)
    t.send("R20240101161050", 0.05)
    s = t.wait(lambda s: s["view"] == HIDDEN, 10)
    check("an alert brings the map back", s["view"] == HIDDEN and s["alarms"] != s0["alarms"])
    t.send("L", 0.2)
    t.wait(lambda s: s["menu"] == "0", 3)
    t.swipe(360, 10, 420)
    while int(t.status()["sound"]) < sound0:
        t.tap(PLUS_X, SOUND_Y)
    t.swipe(360, 200, 40)

    # Timeout
    t.wait(lambda s: s["menu"] == "0", 3)
    open_and_scan()
    s = t.wait(lambda s: s["view"] == HIDDEN, 70)
    check("list closes by itself after a minute", s["view"] == HIDDEN)

    s = t.status()
    print("end:", {k: s[k] for k in ("uptime", "shown", "failed", "wifi", "ssid", "sound", "known", "heap", "uiworst")})
    print("ALL PASS" if not failures else f"{failures} FAILURES")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
