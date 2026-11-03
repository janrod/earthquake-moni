#!/usr/bin/env python3
"""Checks the spinner on opening the earthquake list, and saves screenshots of it and the times.

usage: list_open_test.py PORT OUTDIR
Takes about a minute and restarts the device twice, to get a first visit each time.
"""
import subprocess, sys, time

from menu_test import Tab5

LIST = "1"


def main():
    port, out = sys.argv[1], sys.argv[2]
    t = Tab5(port)
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        print(("ok   " if ok else "FAIL ") + name, detail)
        failures += 0 if ok else 1

    def wait_for(want, seconds):
        end = time.time() + seconds
        while time.time() < end:
            s = t.status()
            if want(s):
                return s
            time.sleep(0.1)
        return None

    def shot(name):
        subprocess.run([sys.executable, "-I", "tools/screenshot.py", port, f"{out}/{name}.ppm"], check=True, stdout=subprocess.DEVNULL)

    def swipe_h(x0, x1, y=600):
        for i in range(11):
            t.press(x0 + (x1 - x0) * i // 10, y)
        t.release()
        # Commands queue behind screen updates, so wait for the slide itself to end.
        time.sleep(0.2)
        wait_for(lambda s: s["qslide"] == "-1", 4)

    def hold_slide():
        for i in range(11):
            t.press(700 + (20 - 700) * i // 10, 600)

    def fresh_start():
        """A restart, then a live connection and a lit screen."""
        nonlocal t
        t.send("X", 0.1)
        time.sleep(4)
        t = Tab5(port)
        s = wait_for(lambda s: int(s["shown"]) > 0, 40)
        if s["backlightoff"] == "1":
            t.tap(360, 900)
        time.sleep(0.5)

    s = t.status()
    print("start:", {k: s[k] for k in ["night", "nightwake", "auto", "brightness", "sound"]})

    fresh_start()
    t0 = time.time()
    hold_slide()
    s = t.status()  # the list takes about a second, so this has to be while the finger is down
    check("the first visit waits", s["qview"] == LIST and s["qwait"] == "1", f'qview={s["qview"]} qwait={s["qwait"]} after {time.time() - t0:.1f} s')
    t.release()
    s = wait_for(lambda s: s["qwait"] == "0", 15)
    took = time.time() - t0
    check("the rows appear", s is not None and s["qlist"] == "3" and int(s["qvis"]) > 0, f"after {took:.1f} s, of 10 s allowed, qfail={s and s['qfail']}")
    time.sleep(0.3)
    shot("list_ago")
    t.tap(360, 300)
    time.sleep(0.2)
    wait_for(lambda s: s["qdetail"] == "3", 8)
    time.sleep(0.3)
    shot("detail_ago")
    t.tap(60, 50)
    time.sleep(0.2)
    t.tap(60, 50)
    wait_for(lambda s: s["qview"] == "0", 12)

    time.sleep(0.5)
    swipe_h(700, 240)
    s = t.status()
    check("coming back soon shows the rows at once", s["qview"] == LIST and s["qwait"] == "0", f'qwait={s["qwait"]}')
    t.tap(60, 50)
    wait_for(lambda s: s["qview"] == "0", 12)

    fresh_start()
    hold_slide()
    shot("list_spinner")  # nearly all of the page is across, and the frame holds during the dump
    t.release()
    s = wait_for(lambda s: s["qwait"] == "0", 15)
    check("the spinner gives way again", s is not None)
    t.tap(60, 50)
    wait_for(lambda s: s["qview"] == "0", 12)

    s = t.status()
    print("end:", {k: s[k] for k in ["night", "nightwake", "auto", "brightness", "sound", "failed", "uiworst", "minheap"]})
    print("ALL PASS" if not failures else f"{failures} FAILED")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
