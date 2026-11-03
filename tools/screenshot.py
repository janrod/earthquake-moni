#!/usr/bin/env python3
"""Sends a serial command to the Tab5 and optionally saves its screen as a PPM.

usage: screenshot.py PORT out.ppm            grab the screen
       screenshot.py PORT - R20240101161030  send a command, print the reply
"""
import os, select, sys, termios, time, tty


def read_some(fd, timeout):
    r, _, _ = select.select([fd], [], [], timeout)
    return os.read(fd, 65536) if r else b""


def main():
    port, out = sys.argv[1], sys.argv[2]
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[2] &= ~termios.HUPCL  # closing the port must not reset the device
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    if out == "-":
        os.write(fd, sys.argv[3].encode())
        end, buf = time.time() + 1.5, b""
        while time.time() < end:
            buf += read_some(fd, 0.2)
        sys.stdout.write(buf.decode("utf-8", "replace"))
        return
    os.write(fd, b"S")
    buf, end, marker = b"", time.time() + 30, b"KMONI-SCREEN "
    while time.time() < end:
        buf += read_some(fd, 0.5)
        i = buf.find(marker)
        if i >= 0 and b"\n" in buf[i:]:
            nl = buf.index(b"\n", i)
            w, h = map(int, buf[i + len(marker):nl].split())
            need = w * h * 2
            data = buf[nl + 1:]
            while len(data) < need and time.time() < end:
                data += read_some(fd, 0.5)
            if len(data) < need:
                sys.exit(f"short read: {len(data)}/{need}")
            with open(out, "wb") as f:
                f.write(b"P6 %d %d 255\n" % (w, h))
                px = bytearray(w * h * 3)
                for p in range(w * h):
                    c = data[2 * p] << 8 | data[2 * p + 1]
                    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
                    px[3 * p] = r << 3 | r >> 2
                    px[3 * p + 1] = g << 2 | g >> 4
                    px[3 * p + 2] = b << 3 | b >> 2
                f.write(px)
            print(f"{w}x{h} -> {out}")
            return
    sys.exit("no screen marker received")


main()
