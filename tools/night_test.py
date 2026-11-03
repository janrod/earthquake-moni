#!/usr/bin/env python3
"""Checks night mode on the Tab5, with simulated times and injected touches.

usage: night_test.py PORT [--no-touch | --wake-only]
Takes about four minutes and restarts the device once. Starts by asking for one real touch, as
injected touches can't show that the touch panel still works with the display off, and stops there
if it doesn't. Mutes the sound while it replays a past alert. Puts the settings back, including if
it fails part way.
--no-touch skips the real touch, for when only the other checks need repeating. --wake-only checks
just the time the screen stays on after a touch, in about a minute, with no real touch.
"""
import datetime, sys, time

from menu_test import MINUS_X, NIGHT_Y, PLUS_X, SOUND_Y, Tab5

HOLD = 30 + 4  # the hold after a touch, and the fade that follows it
ALERT_STAMP = "20240101161100"  # Noto, 2024
MODE_X = {"off": 130, "scheduled": 360, "continuous": 590}
MODE_Y = 165
FIELD_X, FIELD_Y = (185, 535), 325
WAKE_X, WAKE_Y = (335, 645), 445  # minus, plus
KEY_X, KEY_Y = (120, 360, 600), (715, 865, 1015, 1165)
KEYS = {str(d): (KEY_X[(d - 1) % 3], KEY_Y[(d - 1) // 3]) for d in range(1, 10)}
KEYS["0"] = (KEY_X[1], KEY_Y[3])
SAVE = (KEY_X[2], KEY_Y[3])


class WakeChecksDone(Exception):
    pass


def main():
    port = sys.argv[1]
    t = Tab5(port)
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        print(("ok   " if ok else "FAIL ") + name, detail)
        failures += 0 if ok else 1

    def wait_for(want, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if want(t.status()):
                return True
            time.sleep(0.5)
        return False

    def wake():
        if t.status()["backlightoff"] == "1":
            t.tap(360, 900)
            time.sleep(0.3)

    def open_editor():
        wake()
        t.swipe(360, 10, 320)
        t.tap(360, NIGHT_Y)
        return wait_for(lambda s: s["nightview"] == "1", 3)

    def set_mode(mode, start=None, end=None):
        ok = open_editor()
        if start:  # the times can only be typed under Scheduled
            t.tap(MODE_X["scheduled"], MODE_Y)
            t.tap(FIELD_X[0], FIELD_Y)
            for c in start + end:
                t.tap(*KEYS[c])
        t.tap(MODE_X[mode], MODE_Y)
        t.tap(*SAVE)
        return ok and wait_for(lambda s: s["nightview"] == "0", 3)

    def at(hhmm):
        t.send(f"N{hhmm}", 0.3)

    def set_wake(seconds):
        ok = open_editor()
        for _ in range(11):
            t.tap(WAKE_X[0], WAKE_Y)  # down to the 5 s stop, wherever it was
        for _ in range((seconds - 5) // 5):
            t.tap(WAKE_X[1], WAKE_Y)
        t.tap(*SAVE)
        return ok and wait_for(lambda s: s["nightview"] == "0", 3)

    def seconds_until_off():
        start = time.time()
        wait_for(lambda s: s["backlightoff"] == "1", 70)
        return time.time() - start

    s = t.status()
    print("start:", {k: s[k] for k in ["brightness", "level", "sound", "night", "nightstart", "nightend", "backlightoff"]})
    night0, start0, end0, sound0, wake0 = s["night"], int(s["nightstart"]), int(s["nightend"]), int(s["sound"]), int(s["nightwake"])
    high = int(s["brightness"]) * 255 // 100

    try:
        if "--wake-only" in sys.argv:
            check("saves 5 s", set_wake(5) and t.status()["nightwake"] == "5")
            check("saves Continuous", set_mode("continuous"))
            took = seconds_until_off()
            check("off about 5 s after the last touch", 3.5 < took < 7.5, round(took, 1))
            t.tap(360, 900)
            s = t.status()
            check("a touch wakes it", s["backlightoff"] == "0", s["level"])
            took = seconds_until_off()
            check("and it's off 5 s later", 3.5 < took < 7.5, round(took, 1))
            check("saves 15 s", set_wake(15) and t.status()["nightwake"] == "15")
            took = seconds_until_off()
            check("off about 15 s after the last touch", 13 < took < 18, round(took, 1))
            t.send("X", 0.1)
            time.sleep(4)
            t = Tab5(port)
            check("kept across a restart", t.status()["nightwake"] == "15")
            raise WakeChecksDone
        check("saves a schedule", set_mode("scheduled", "2315", "0645"))
        s = t.status()
        check("schedule stored", s["night"] == "1" and s["nightstart"] == "1395" and s["nightend"] == "405", s["nightstart"])

        # The real finger comes first: without it, nothing else matters.
        at("0000")
        check("backlight switches off", wait_for(lambda s: s["backlightoff"] == "1" and s["level"] == "0", HOLD + 5))
        if "--no-touch" not in sys.argv:
            before = int(t.status()["touches"])
            print(">>> Touch the Tab5's screen now, once. Waiting up to 90 s.", flush=True)
            seen = wait_for(lambda s: int(s["touches"]) > before, 90)
            check("a real touch is seen with the display off", seen)
            if not seen:
                sys.exit(1)
            s = t.status()
            check("and wakes it", s["backlightoff"] == "0" and int(s["level"]) > 0, s["level"])
            print(">>> Hands off from here. Does the map look right? It should go black in about 30 s.", flush=True)

        at("1200")
        check("awake outside the schedule", wait_for(lambda s: s["backlightoff"] == "0" and int(s["level"]) > 0, 3))
        at("0000")
        frames = int(t.status()["shown"])
        check("off inside it", wait_for(lambda s: s["backlightoff"] == "1" and s["level"] == "0", HOLD + 5))
        time.sleep(4)
        s = t.status()
        check("still fetching while off", int(s["shown"]) > frames, f'{frames}->{s["shown"]}')
        at("0644")
        check("the last minute is still off", t.status()["backlightoff"] == "1")
        at("0645")
        check("on again at the end time", wait_for(lambda s: s["backlightoff"] == "0" and int(s["level"]) > 0, 3))

        at("0000")
        check("off again", wait_for(lambda s: s["backlightoff"] == "1", 8))
        # A swipe that would open the panel only wakes the screen.
        t.swipe(360, 10, 320)
        s = t.status()
        check("a touch wakes it", s["backlightoff"] == "0" and s["level"] == str(high), s["level"])
        check("the waking touch does nothing else", s["menu"] == "0", s["menu"])
        t.swipe(360, 10, 320)
        check("the next touch works normally", t.status()["menu"] != "0")
        t.swipe(360, 200, 40)
        check("off after the hold", wait_for(lambda s: s["backlightoff"] == "1", HOLD + 5))

        # The alert: muted, because this replay really raises the alarm.
        wake()
        t.swipe(360, 10, 320)
        for _ in range(11):
            if int(t.status()["sound"]) > 0:
                t.tap(MINUS_X, SOUND_Y)
        if int(t.status()["sound"]) > 0:
            sys.exit("couldn't mute: not replaying the alert")
        t.tap(360, 900)
        wait_for(lambda s: s["menu"] == "0", 12)
        check("off before the alert", wait_for(lambda s: s["backlightoff"] == "1", HOLD + 5))
        alarms = int(t.status()["alarms"])
        t.send(f"R{ALERT_STAMP}", 0.3)
        check("an alert wakes it to full", wait_for(lambda s: s["level"] == "255" and s["backlightoff"] == "0", 30))
        check("the alarm was raised, silently", int(t.status()["alarms"]) > alarms)
        t.send("L", 0.3)
        check("off again after the alert", wait_for(lambda s: s["backlightoff"] == "1", 90))

        check("saves Continuous", set_mode("continuous"))
        at("----")
        check("off at any hour", wait_for(lambda s: s["backlightoff"] == "1", HOLD + 5))

        # A restart of our own stays dark, knowing the time at once.
        t.send("X", 0.1)
        time.sleep(4)
        t = Tab5(port)
        s = t.status()
        check("restarted", int(s["uptime"].rstrip("s")) < 30, s["uptime"])
        wait_for(lambda s: int(s["dimmin"]) >= 0, 2)  # the first reply can come before the first update
        s = t.status()
        check("clock kept across the restart", int(s["dimmin"]) >= 0, s["dimmin"])
        check("dark after the restart", s["backlightoff"] == "1", s["level"])
    except WakeChecksDone:
        pass
    finally:
        at("----")
        t.send("L", 0.3)
        names = {"0": "off", "1": "scheduled", "2": "continuous"}
        hhmm = lambda m: f"{m // 60:02d}{m % 60:02d}"
        set_mode(names[night0], hhmm(start0), hhmm(end0))
        set_wake(wake0)
        wake()
        t.swipe(360, 10, 320)
        for _ in range(11):
            if int(t.status()["sound"]) < sound0:
                t.tap(PLUS_X, SOUND_Y)
        t.tap(360, 900)
        wait_for(lambda s: s["menu"] == "0", 12)

    s = t.status()
    check("settings put back", s["night"] == night0 and int(s["sound"]) == sound0 and int(s["nightwake"]) == wake0, f'night={s["night"]} sound={s["sound"]} wake={s["nightwake"]}')
    print("end:", {k: s[k] for k in ["brightness", "level", "sound", "night", "backlightoff", "failed", "minheap", "uiworst"]})
    print("ALL PASS" if not failures else f"{failures} FAILED")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
