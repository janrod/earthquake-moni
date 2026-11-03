#!/usr/bin/env python3
"""Checks the night dimming on the Tab5, with simulated times and injected touches.

usage: dim_test.py PORT
Takes about three minutes. Mutes the sound while it replays a past alert, and puts it back.
"""
import datetime, sys, time

from menu_test import MINUS_X, PLUS_X, SOUND_Y, Tab5

LOW = 10 * 255 // 100
HOLD = 30 + 3  # the hold after a touch, and the fade that follows it
ALERT_STAMP = "20240101161100"  # Noto, 2024


def jst_minute():
    now = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=9)))
    return now.hour * 60 + now.minute


def main():
    t = Tab5(sys.argv[1])
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        print(("ok   " if ok else "FAIL ") + name, detail)
        failures += 0 if ok else 1

    def level():
        return int(t.status()["level"])

    def wait_for(want, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if want(t.status()):
                return True
            time.sleep(0.5)
        return False

    def at(hhmm):
        t.send(f"N{hhmm}", 0.3)

    check("clock syncs", wait_for(lambda s: int(s["dimmin"]) >= 0 and int(s["shown"]) > 0, 60))
    s = t.status()
    print("start:", {k: s[k] for k in ["brightness", "level", "sound", "dimmin", "sunrise", "sunset", "touches"]})
    set_percent, sound0 = int(s["brightness"]), int(s["sound"])
    high = set_percent * 255 // 100
    rise, sunset = int(s["sunrise"]), int(s["sunset"])

    def scheduled(m):
        if m >= 22 * 60 or m < 270:
            return LOW
        end, start = max(rise, 300), min(sunset, 21 * 60 + 30)
        if m < end:
            return LOW + (high - LOW) * (m - 270) // (end - 270)
        if m >= start:
            return high - (high - LOW) * (m - start) // (22 * 60 - start)
        return high

    time.sleep(3)  # the clock arriving at night starts a fade
    s = t.status()
    check("uses JST from the server", abs(int(s["dimmin"]) - jst_minute()) <= 1, f'{s["dimmin"]} vs {jst_minute()}')
    check("level right for the real time", abs(int(s["level"]) - scheduled(int(s["dimmin"]))) <= 1, s["level"])

    try:
        at("1200")
        check("noon is the set brightness", level() == high)
        at("2300")
        first = level()
        check("going dim fades", first > LOW, first)
        time.sleep(3)
        check("23:00 is the lowest", level() == LOW)
        for hhmm, m in [("2200", 1320), ("0000", 0), ("0429", 269), ("0430", 270), ("2100", 1260), ("1900", 1140), ("0500", 300)]:
            at(hhmm)
            time.sleep(2.5)
            got = level()
            check(f"{hhmm[:2]}:{hhmm[2:]} on the curve", abs(got - scheduled(m)) <= 1, f"{got} vs {scheduled(m)}")

        at("2300")
        time.sleep(3)
        t.tap(360, 500)
        check("touch goes straight to the set brightness", level() == high)
        time.sleep(24)
        check("still up after 24 s", level() == high)
        time.sleep(9)
        check("back to the lowest after the hold", level() == LOW, level())

        # The alert: muted, because this replay really raises the alarm.
        t.swipe(360, 10, 320)
        while int(t.status()["sound"]) > 0:
            t.tap(MINUS_X, SOUND_Y)
        t.tap(360, 900)
        check("muted", wait_for(lambda s: s["menu"] == "0" and s["sound"] == "0", 5))
        alarms = int(t.status()["alarms"])
        time.sleep(HOLD)
        check("dim before the alert", level() == LOW, level())
        t.send(f"R{ALERT_STAMP}", 0.3)
        check("alert goes to full", wait_for(lambda s: s["level"] == "255", 30))
        s = t.status()
        check("the alarm was raised, silently", int(s["alarms"]) > alarms and s["playing"] == "0", f'{s["alarms"]} playing={s["playing"]}')
        t.send("L", 0.3)
        check("fades back after the alert", wait_for(lambda s: int(s["level"]) == LOW, 75), level())
    finally:
        at("----")
        t.send("L", 0.3)
        t.swipe(360, 10, 320)
        while int(t.status()["sound"]) < sound0:
            t.tap(PLUS_X, SOUND_Y)
        t.tap(360, 900)
        wait_for(lambda s: s["menu"] == "0", 5)

    time.sleep(HOLD)
    s = t.status()
    check("sound put back", int(s["sound"]) == sound0, s["sound"])
    check("brightness unchanged", int(s["brightness"]) == set_percent, s["brightness"])
    check("back on the real clock", abs(int(s["dimmin"]) - jst_minute()) <= 1, s["dimmin"])
    check("level right for the real time", abs(int(s["level"]) - scheduled(int(s["dimmin"]))) <= 1, f'{s["level"]} vs {scheduled(int(s["dimmin"]))}')
    print("end:", {k: s[k] for k in ["brightness", "level", "sound", "auto", "dimmin", "shown", "failed", "minheap", "uiworst", "ssid"]})
    print("ALL PASS" if not failures else f"{failures} FAILED")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
