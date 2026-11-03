#!/usr/bin/env python3
"""Drives the swipe-down menu on the Tab5 through injected touches, and checks the results.

usage: menu_test.py PORT
Restores the brightness and sound values it finds, then closes the panel.
"""
import os, re, select, sys, termios, time, tty

MINUS_X, PLUS_X, BRIGHT_Y, SOUND_Y, AUTO_Y, WIFI_Y, LOCATION_Y, NIGHT_Y = 116, 604, 74, 206, 338, 456, 548, 640
PANEL_H = 704


class Tab5:
    def __init__(self, port):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        attrs[2] &= ~termios.HUPCL  # closing the port must not reset the device
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def note(self, data):
        """Keeps the device's own log lines, for reporting timings."""
        self.log = getattr(self, "log", []) + [l.decode("utf-8", "replace").strip() for l in data.split(b"\n")
                                                 if l.startswith((b"jma ", b"list parsed", b"regions ", b"detail ", b"connect"))]

    def send(self, s, wait=0.04):
        os.write(self.fd, s.encode())
        time.sleep(wait)

    def press(self, x, y):
        self.send(f"P{x:04d}{y:04d}")

    def release(self):
        self.send("U")

    def swipe(self, x, y0, y1, steps=10):
        for i in range(steps + 1):
            self.press(x, y0 + (y1 - y0) * i // steps)
        self.release()
        time.sleep(0.5)

    def tap(self, x, y):
        self.press(x, y)
        time.sleep(0.1)
        self.release()
        time.sleep(0.2)

    def status(self):
        # The device drops serial output rather than ever waiting on USB, so a reply can be lost.
        end = time.time() + 8
        while time.time() < end:
            while select.select([self.fd], [], [], 0)[0]:
                self.note(os.read(self.fd, 65536))
            self.send("I", 0.0)
            buf, retry = b"", time.time() + 1
            while time.time() < retry:
                if select.select([self.fd], [], [], 0.1)[0]:
                    data = os.read(self.fd, 65536)
                    self.note(data)
                    buf += data
                for line in buf.split(b"\n")[:-1]:
                    if line.startswith(b"board=") and b" ssid=" in line and line.count(b"board=") == 1:
                        head, ssid = line.rstrip().split(b" ssid=", 1)
                        status = {k.decode(): v.decode() for k, v in re.findall(rb"(\w+)=([\w.-]+)", head)}
                        status["ssid"] = ssid.decode("utf-8", "replace")
                        if "uptime" in status and "wifi" in status:
                            return status
        sys.exit("no status reply")


def main():
    t = Tab5(sys.argv[1])
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        print(("ok   " if ok else "FAIL ") + name, detail)
        failures += 0 if ok else 1

    s = t.status()
    bright0, sound0 = int(s["brightness"]), int(s["sound"])
    print("start:", s)
    check("panel starts closed", s["menu"] == "0")

    t.swipe(360, 400, 800)
    check("mid-screen swipe ignored", t.status()["menu"] == "0")
    t.swipe(360, 10, 60)
    check("short swipe springs back", t.status()["menu"] == "0")
    t.swipe(360, 10, 320)
    s = t.status()
    check("swipe from top opens", s["menu"] == str(PANEL_H), s["menu"])

    # Move away from any limit first so both directions can be seen.
    if bright0 >= 100:
        t.tap(MINUS_X, BRIGHT_Y)
    b = int(t.status()["brightness"])
    t.tap(PLUS_X, BRIGHT_Y)
    s = t.status()
    check("plus raises brightness", int(s["brightness"]) == b + 10, s["brightness"])
    check("backlight follows", abs(int(s["level"]) - int(s["brightness"]) * 255 // 100) <= 1, s["level"])
    t.tap(MINUS_X, BRIGHT_Y)
    check("minus lowers brightness", int(t.status()["brightness"]) == b)

    if sound0 >= 100:
        t.tap(MINUS_X, SOUND_Y)
    v = int(t.status()["sound"])
    t.tap(PLUS_X, SOUND_Y)
    s = t.status()
    check("plus raises sound", int(s["sound"]) == v + 10, s["sound"])
    check("speaker volume follows", abs(int(s["volume"]) - int(s["sound"]) * 255 // 100) <= 1, s["volume"])
    t.tap(MINUS_X, SOUND_Y)
    check("minus lowers sound", int(t.status()["sound"]) == v)

    frames0 = int(t.status()["shown"])
    time.sleep(3)
    s = t.status()
    check("map keeps updating behind open panel", int(s["shown"]) >= frames0 + 2, f'{frames0}->{s["shown"]}')

    # Put back what we found.
    while int(t.status()["brightness"]) > bright0:
        t.tap(MINUS_X, BRIGHT_Y)
    while int(t.status()["brightness"]) < bright0:
        t.tap(PLUS_X, BRIGHT_Y)
    while int(t.status()["sound"]) > sound0:
        t.tap(MINUS_X, SOUND_Y)
    while int(t.status()["sound"]) < sound0:
        t.tap(PLUS_X, SOUND_Y)

    t.swipe(360, 200, 40)
    check("swipe up closes", t.status()["menu"] == "0")
    s = t.status()
    print("end:", s)
    print("ALL PASS" if not failures else f"{failures} FAILURES")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
