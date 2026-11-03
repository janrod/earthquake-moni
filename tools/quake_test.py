#!/usr/bin/env python3
"""Drives the earthquake pages on the Tab5 through injected touches.

usage: quake_test.py PORT OUTDIR
Restores the auto-return setting it finds. Saves screenshots to OUTDIR.
"""
import os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(__file__))
from menu_test import AUTO_Y, LOCATION_Y, MINUS_X, PANEL_H, PLUS_X, Tab5
from wifi_test import Driver

HIDDEN, LIST, DETAIL = "0", "1", "2"
LOADING, FAILED, READY = "1", "2", "3"


LIST_TOP = 176
FILTER_LABELS = ["All", "1", "2", "3", "4", "5-", "5+", "6-", "6+", "7"]


def row_y(i):
    return LIST_TOP + i * 118 + 59


def filter_xy(button):
    return button * 72 + 36, 138


def main():
    port, out = sys.argv[1], sys.argv[2]
    t = Driver(port)
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        print(("ok   " if ok else "FAIL ") + name, detail)
        failures += 0 if ok else 1

    def shot(name):
        r = subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "screenshot.py"), port, f"{out}/{name}.ppm"],
                           capture_output=True, text=True)
        if r.returncode:
            print(f"     screenshot {name} failed: {r.stderr.strip()[-200:]}")
            return
        subprocess.run(["sips", "-s", "format", "png", f"{out}/{name}.ppm", "--out", f"{out}/{name}.png"], capture_output=True)

    def rank_counts():
        """Rows the list holds at each level, built-in ones included."""
        for line in t.lines("Z", "ranks"):
            return [0] + [int(n) for n in line.split()[1:]]
        return None

    def rows():
        found = []
        for l in t.lines("Q", "row"):
            p = l.split(" ", 4)
            if len(p) >= 4 and p[1] != "end":
                found.append((int(p[1]), int(p[2]), int(p[3]), p[4] if len(p) == 5 else ""))
        return found

    def set_auto(seconds):
        """Walks the setting with the panel open, then closes the panel."""
        t.swipe(360, 10, 500)
        stops = [0, 5, 10, 15, 20, 30, 60, 120]
        while int(t.status()["auto"]) > seconds:
            t.tap(MINUS_X, AUTO_Y)
        while int(t.status()["auto"]) < seconds:
            t.tap(PLUS_X, AUTO_Y)
        assert seconds in stops
        t.swipe(360, 200, 40)

    s0 = t.status()
    auto0 = int(s0["auto"])
    if s0["qview"] != HIDDEN:  # left open by an earlier run
        for _ in range(2):
            for i in range(11):
                t.press(100 + 50 * i, 600)
            t.release()
            time.sleep(0.6)
    print("start:", {k: s0[k] for k in ("uptime", "wifi", "ssid", "shown", "failed", "auto", "heap", "minheap")})

    # Panel setting; Off lets the slower steps below run without a deadline.
    t.swipe(360, 10, 500)
    shot("panel")
    t.swipe(360, 200, 40)
    set_auto(0)
    check("auto-return can be turned off", t.status()["auto"] == "0")

    # Gesture
    t.swipe(500, 600, 100)  # vertical: nothing
    check("vertical swipe does nothing", t.status()["qview"] == HIDDEN)

    def swipe_h(x0, x1, y=600):
        for i in range(11):
            t.press(x0 + (x1 - x0) * i // 10, y)
        t.release()
        # Commands queue behind screen updates, so wait for the slide itself to end.
        time.sleep(0.2)
        t.wait(lambda s: s["qslide"] == "-1", 4)

    swipe_h(500, 100)
    check("a leftward swipe away from the edge does nothing", t.status()["qview"] == HIDDEN)
    swipe_h(100, 560)
    check("a rightward swipe on the live page does nothing", t.status()["qview"] == HIDDEN)
    swipe_h(700, 620)
    s = t.status()
    check("a short pull from the edge springs back", s["qview"] == HIDDEN, f'qview={s["qview"]} qslide={s["qslide"]}')
    n0 = int(t.status()["shown"]); time.sleep(2.5)
    check("and the live page carries on", int(t.status()["shown"]) >= n0 + 1)

    # Mid-drag, both pages should be on screen, side by side.
    for x in (700, 680, 620, 520, 420):
        t.press(x, 600)
    time.sleep(2.5)
    s = t.status()
    check("the page follows the finger", s["qslide"] == "280", "qslide=" + s["qslide"])
    shot("slide_mid")
    t.release()
    time.sleep(0.6)
    s = t.status()
    check("releasing past the point finishes the slide", s["qview"] == LIST and s["qslide"] == "-1", f'qview={s["qview"]}')
    swipe_h(100, 600)
    check("swiping right returns to live", t.status()["qview"] == HIDDEN)

    # HTTPS and the list, while measuring the live map.
    a = t.status()
    t0 = time.time()
    swipe_h(700, 240)
    s = t.status()
    check("dragging in from the right edge opens the list", s["qview"] == LIST, f'qview={s["qview"]} menu={s["menu"]}')
    s = t.wait(lambda s: s["qlist"] in (READY, FAILED), 40)
    check("list loads over HTTPS", s["qlist"] == READY and int(s["qrows"]) > 0, f'rows={s["qrows"]} after {time.time() - t0:.1f}s, qfail={s["qfail"]}')
    shot("list")
    r = rows()
    check("row listing has the live list", 0 < len(r) <= 256 and len(r) < int(s["qrows"]), f'{len(r)} of {s["qrows"]}')

    # Strongest row on the first screen, as that has the most on its map.
    visible = [x for x in r if x[0] < 8]
    pick = max(visible, key=lambda x: (x[2], x[1]))
    print("opening row", pick)
    t.tap(300, row_y(pick[0]))
    s = t.status()
    check("tapping a row opens its detail", s["qview"] == DETAIL, "qview=" + s["qview"])
    t0 = time.time()
    s = t.wait(lambda s: s["qdetail"] in (READY, FAILED), 45)
    check("detail and map load", s["qdetail"] == READY, f"after {time.time() - t0:.1f}s, qfail={s['qfail']}")
    shot("detail")

    # A second report needs no outline download.
    t.tap(70, 50)
    check("back returns to the list", t.status()["qview"] == LIST)
    r = rows()
    other = next((x for x in r if x[0] < 8 and x[0] != pick[0] and x[2]), None)
    if other:
        t.tap(300, row_y(other[0]))
        t0 = time.time()
        s = t.wait(lambda s: s["qdetail"] in (READY, FAILED), 30)
        check("second report loads", s["qdetail"] == READY, f"after {time.time() - t0:.1f}s")
        shot("detail2")
        swipe_h(150, 600)
        check("swiping right on a detail returns to the list", t.status()["qview"] == LIST)

    b = t.status()
    frames, secs = int(b["shown"]) - int(a["shown"]), int(b["uptime"].rstrip("s")) - int(a["uptime"].rstrip("s"))
    check("live map kept updating", frames >= secs * 0.9 and b["failed"] == a["failed"], f'{frames} frames in {secs}s, failed {a["failed"]}->{b["failed"]}')

    # Scrolling
    for i in range(11):
        t.press(360, 1000 - i * 60)
    t.release()
    time.sleep(0.4)
    check("dragging scrolls without opening", t.status()["qview"] == LIST)
    time.sleep(2)
    shot("list_scrolled")

    # A row can be opened after scrolling, and it's the row under the finger.
    s = t.status()
    scroll = int(s["qscroll"])
    check("the list really scrolled", scroll > 0, f"scroll={scroll}")
    index = (scroll + 400) // 118  # a row fully on screen
    t.tap(300, LIST_TOP + index * 118 - scroll + 59)
    s = t.status()
    check("tapping after scrolling opens that row", s["qview"] == DETAIL and int(s["qselected"]) == index, f'qview={s["qview"]} selected={s["qselected"]} wanted={index}')
    s = t.wait(lambda s: s["qdetail"] in (READY, FAILED), 25)
    t.tap(70, 50)
    check("back again", t.status()["qview"] == LIST)

    # Filter. Expected counts come from the device's own list of rows. All has one extra line,
    # the divider above the older rows that are only the stronger levels, and also shows rows
    # with no intensity yet, which rank_counts() leaves out.
    r = rows()
    ranks = [x[1] for x in r]
    counts = rank_counts()
    total = sum(counts)
    check("the list holds the built-in earthquakes", total > len(r) + 15000 and counts[9] >= 8, counts)
    def tap_filter(b):
        x, y = filter_xy(b)
        t.tap(x, y)
    check("filter starts at All", t.status()["qvis"] == str(int(t.status()["qrows"]) + 1) and t.status()["qfilter"] == "0", t.status()["qvis"])
    level = max(set(x for x in ranks if x), key=ranks.count)  # the commonest level
    tap_filter(level)
    s = t.status()
    check(f"filtering to {FILTER_LABELS[level]}", int(s["qvis"]) in (counts[level], counts[level] + 1), f'{s["qvis"]} of {total}')
    time.sleep(2)
    shot("filter_one")
    stronger = sorted(set(x for x in ranks if x and x != level))
    if stronger:
        tap_filter(stronger[-1])
        s = t.status()
        check("toggles combine", int(s["qvis"]) in (counts[level] + counts[stronger[-1]], counts[level] + counts[stronger[-1]] + 1), s["qvis"])
        time.sleep(2)
        shot("filter_two")
        tap_filter(level)
        s = t.status()
        check("a toggle turns back off", int(s["qvis"]) in (counts[stronger[-1]], counts[stronger[-1]] + 1), s["qvis"])
        # The strongest earthquake: opens by its real row, however the filter renumbers it.
        t.tap(300, row_y(0))
        s = t.status()
        check("a filtered row opens the right earthquake", int(s["qselected"]) == ranks.index(stronger[-1]), f'{s["qselected"]}')
        t.wait(lambda s: s["qdetail"] in (READY, FAILED), 25)
        time.sleep(3)  # let the device's log go quiet, so it can't land inside the screenshot
        shot("detail_strongest")
        t.tap(70, 50)
    empty = next((b for b in range(1, 3) if not counts[b]), None)
    tap_filter(0)
    check("All shows everything again", t.status()["qvis"] == str(int(t.status()["qrows"]) + 1))
    if empty:
        tap_filter(empty)
        s = t.status()
        check("an empty level shows nothing", s["qvis"] == "0" and s["qview"] == LIST)
        time.sleep(2)
        shot("filter_empty")
        tap_filter(empty)
    check("back to All", t.status()["qfilter"] == "0")

    # The case that started all this: 7 alone, then 6+ too, with no loading at any point.
    tap_filter(9)
    s = t.status()
    check("sevens show at once, back to the start of the records", int(s["qvis"]) == counts[9] and s["since"].startswith("1919-01-01") and s["qolder"] != LOADING, f'qvis={s["qvis"]} since={s["since"]}')
    tap_filter(8)
    s = t.status()
    check("turning on 6+ shows those at once too", int(s["qvis"]) == counts[9] + counts[8] and s["since"].startswith("1919-01-01"), s["qvis"])
    time.sleep(2)
    shot("strong_levels")
    t.tap(300, row_y(0))
    s = t.wait(lambda s: s["qdetail"] in (READY, FAILED), 45)
    check("a built-in earthquake opens with its map", s["qview"] == DETAIL and s["qdetail"] == READY, f'qview={s["qview"]} qdetail={s["qdetail"]}')
    time.sleep(3)
    shot("strong_detail")
    t.tap(70, 50)
    tap_filter(8)
    tap_filter(9)

    # A weak level: still loads on request, and then the list says it reaches further back.
    def tap_list_row(index):
        """Drags until the row is on screen, then taps it."""
        for _ in range(80):
            y = LIST_TOP + index * 118 - int(t.status()["qscroll"])
            if LIST_TOP < y and y + 118 < 1236:
                break
            for i in range(11):
                t.press(360, 1000 - i * 60)
            t.release()
            time.sleep(0.2)
        t.tap(300, y + 59)

    tap_filter(1)
    s = t.status()
    before = s["since"]
    check("level 1 only reaches the end of the live list", not before.startswith("1919"), before)
    tap_list_row(int(s["qvis"]) if int(s["qvis"]) else 1)
    s = t.wait(lambda s: s["qolder"] in (READY, FAILED), 30)
    check("older earthquakes load", s["qolder"] == READY, f'qolder={s["qolder"]} rows={s["older"]} since={s["since"]}')
    check("the list says it now reaches further back", s["since"] < before, f'{before} -> {s["since"]}')
    time.sleep(3)
    shot("older_list")
    tap_filter(1)

    # With everything on: the divider marks where the weak levels stop, and is what loads more.
    tap_filter(0)
    s = t.status()
    check("older rows join the full list", int(s["qvis"]) == int(s["qrows"]) + 1 and int(s["qdiv"]) > 0, f'{s["qvis"]} = {s["qrows"]} + divider at {s["qdiv"]}')
    before, divider = s["since"], int(s["qdiv"])
    tap_list_row(divider)
    s = t.wait(lambda s: s["qolder"] in (READY, FAILED), 30)
    check("the divider loads more", s["qolder"] == READY and s["since"] < before and s["qview"] == LIST, f'{before} -> {s["since"]}')
    check("and moves down", int(s["qdiv"]) > divider, f'{divider} -> {s["qdiv"]}')
    time.sleep(3)
    shot("divider")
    swipe_h(150, 300)
    check("a short pull back stays on the list", t.status()["qview"] == LIST)
    swipe_h(150, 600)
    check("swiping right returns to live", t.status()["qview"] == HIDDEN)

    # Location: opening it shows what's saved, and Back changes nothing.
    s = t.status()
    home = (s["home"], s["homelat"], s["homelon"])
    t.swipe(360, 10, 500)
    t.wait(lambda s: s["menu"] == str(PANEL_H), 3)
    t.tap(360, LOCATION_Y)
    s = t.status()
    check("the Location row opens its screen", s["locview"] == "1")
    time.sleep(1)
    shot("location")
    t.tap(70, 50)
    s = t.status()
    check("back leaves the location unchanged", s["locview"] == "0" and (s["home"], s["homelat"], s["homelon"]) == home, str(home))

    # Auto-return at the default.
    set_auto(10)
    swipe_h(700, 240)
    check("list opens again", t.status()["qview"] == LIST)
    t0 = time.time()
    s = t.wait(lambda s: s["qview"] == HIDDEN, 14)
    took = time.time() - t0
    check("returns to live after 10 s", s["qview"] == HIDDEN and 8.5 <= took <= 12, f"{took:.1f}s")
    n0 = int(t.status()["shown"]); time.sleep(3)
    check("live page resumes", int(t.status()["shown"]) >= n0 + 2)
    shot("live")

    set_auto(5)
    swipe_h(700, 240)
    t0 = time.time()
    s = t.wait(lambda s: s["qview"] == HIDDEN, 9)
    took = time.time() - t0
    check("the setting changes the time", s["qview"] == HIDDEN and 3.5 <= took <= 7, f"{took:.1f}s")

    # An alert returns at once, even with auto-return off. Muted, so silent.
    set_auto(0)
    from menu_test import SOUND_Y
    t.swipe(360, 10, 500)
    sound0 = int(t.status()["sound"])
    while int(t.status()["sound"]) > 0:
        t.tap(MINUS_X, SOUND_Y)
    t.swipe(360, 200, 40)
    swipe_h(700, 240)
    check("list up before the alert", t.status()["qview"] == LIST)
    t.send("R20240101161050", 0.05)
    s = t.wait(lambda s: s["qview"] == HIDDEN, 10)
    check("an alert returns to the map", s["qview"] == HIDDEN)
    t.send("L", 0.2)
    time.sleep(0.5)
    t.swipe(360, 10, 500)
    while int(t.status()["sound"]) < sound0:
        t.tap(PLUS_X, SOUND_Y)
    t.swipe(360, 200, 40)

    set_auto(auto0)
    s = t.status()
    print("end:", {k: s[k] for k in ("uptime", "wifi", "shown", "failed", "qfail", "auto", "sound", "heap", "minheap", "psram", "uiworst")})
    print("device log:")
    for l in t.log:
        print("  " + l)
    print("ALL PASS" if not failures else f"{failures} FAILURES")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
