#!/usr/bin/env python3
"""Drive the digicore-c3 test console and check the display/input invariants.

The handset logs to the same UART this script talks on, so every command is
answered with a short ACK/STAT line that is easy to pick out of the stream.

    python tools/c3_autotest.py COM4

Exit code 0 means every check passed.
"""
import re
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover - the IDF python has pyserial
    sys.stderr.write("pyserial missing: run this with the ESP-IDF python\n")
    raise SystemExit(2)


class Link:
    def __init__(self, port, baud=115200):
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.buf = ""
        self.lines = []

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def pump(self, seconds):
        """Read for `seconds`, keeping every complete line."""
        end = time.time() + seconds
        while time.time() < end:
            chunk = self.ser.read(4096)
            if not chunk:
                continue
            self.buf += chunk.decode("utf-8", "replace")
            while "\n" in self.buf:
                line, self.buf = self.buf.split("\n", 1)
                self.lines.append(line.rstrip("\r"))

    def send(self, cmd, wait=0.35):
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()
        self.pump(wait)

    def take(self, prefix, wait=0.6):
        """Send nothing, wait, return the last line starting with `prefix`."""
        start = len(self.lines)
        self.pump(wait)
        for line in reversed(self.lines[start:]):
            if prefix in line:
                return line
        return None


def stat_fields(line):
    out = {}
    for key, val in re.findall(r"(\w+)=([0-9A-Fa-f]+)", line or ""):
        out[key] = int(val, 16) if key in ("pc", "sr") else int(val)
    return out


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
    link = Link(port)
    failures = []

    def check(name, ok, detail=""):
        print(("PASS  " if ok else "FAIL  ") + name + (("  " + detail) if detail else ""))
        if not ok:
            failures.append(name)

    try:
        link.pump(1.5)
        link.send("V")
        ver = link.take("VER", 0.8)
        check("console answers", ver is not None, ver or "no VER line")

        # --- 1. idle must not spam the panel -------------------------------
        link.send("S")
        a = stat_fields(link.take("STAT", 1.0))
        link.pump(2.0)
        link.send("S")
        b = stat_fields(link.take("STAT", 1.0))
        idle_blits = b.get("blits", -1) - a.get("blits", -1)
        check("idle stays quiet", 0 <= idle_blits <= 2,
              "blits +%d over 2 s" % idle_blits)

        # --- 2. every press must reach the guest ---------------------------
        base = b.get("blits", 0)
        for i in range(3):
            link.send("K 1 8", 0.1)
            link.pump(0.9)
        link.send("S")
        c = stat_fields(link.take("STAT", 1.0))
        press_blits = c.get("blits", 0) - base
        check("three presses all land", press_blits >= 3,
              "blits +%d for 3 presses" % press_blits)

        # --- 3. the safety refresh must fire on a static screen ------------
        base = c.get("blits", 0)
        link.pump(6.0)          # 256 frames at 60 fps = 4.3 s
        link.send("S")
        d = stat_fields(link.take("STAT", 1.0))
        refresh = d.get("blits", 0) - base
        check("guard refresh fires when static", refresh >= 1,
              "blits +%d over 6 s idle" % refresh)

        # --- 4. no transfer errors ----------------------------------------
        check("no panel errors", d.get("errs", 1) == 0, "errs=%d" % d.get("errs", -1))

        # --- 5. frame rate -------------------------------------------------
        link.pump(1.2)
        fps = [float(m) for m in re.findall(r"guest_fps=([\d.]+)", "\n".join(link.lines[-40:]))]
        if fps:
            check("frame rate holds", min(fps[-3:]) >= 59.0, "last=%s" % fps[-3:])

        # --- 6. framebuffer CRC is stable while static ---------------------
        link.send("C")
        crc1 = link.take("FB_CRC", 0.8)
        link.pump(2.5)
        link.send("C")
        crc2 = link.take("FB_CRC", 0.8)
        check("static framebuffer is stable", crc1 == crc2, "%s vs %s" % (crc1, crc2))

        # --- 7. alignment pattern can be painted ---------------------------
        link.send("G 1")
        ack = link.take("ACK G", 0.8)
        link.pump(0.4)
        link.send("S")
        pat = stat_fields(link.take("STAT", 1.0))
        check("pattern paints", ack is not None and pat.get("blits", 0) > 0,
              ack or "no ACK")
        link.send("L 14 0 24")
        row = link.take("ROW 14", 0.8)
        check("pattern row readable", row is not None, (row or "")[:70])
        link.send("G 0")
        link.take("ACK G", 0.6)
    finally:
        link.close()

    print()
    if failures:
        print("FAILED: " + ", ".join(failures))
        raise SystemExit(1)
    print("ALL CHECKS PASSED")


if __name__ == "__main__":
    main()
