#!/usr/bin/env python3
"""Round-3 clock / save acceptance harness for the digicore-c3 console.

    python tools/c3_clock_audit.py check            # one CLK+STAT snapshot
    python tools/c3_clock_audit.py anchor           # t <now>, then 60 s of CLK
    python tools/c3_clock_audit.py load             # 60 s with keys, worst |dev|
    python tools/c3_clock_audit.py snap             # force a snapshot (D)
    python tools/c3_clock_audit.py u                # runtime load (U)

Port defaults to COM4; override with the C3_PORT environment variable.

The clock is expected to hold |dev| <= 2 s: the firmware compares the guest's
software RTC with a host-side Unix anchor every 250 ms and only writes the
registers when the guest has drifted outside +/-1 s
(docs/AUDIT_SAVE_CLOCK_2026-11-24_R3.md).
"""
import datetime
import os
import re
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    sys.stderr.write("pyserial missing: run this with the ESP-IDF python\n")
    raise SystemExit(2)

PORT = os.environ.get("C3_PORT", "COM4")
buf = ""


def open_port():
    s = serial.Serial()
    s.port = PORT
    s.baudrate = 115200
    s.timeout = 0.2
    # Keep the control lines low: asserting them resets the board on this port.
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def read_for(s, seconds):
    global buf
    t_end = time.time() + seconds
    while time.time() < t_end:
        d = s.read(8192)
        if d:
            buf += d.decode("utf-8", "replace")
    lines = buf.splitlines()
    buf = lines[-1] if lines and not buf.endswith("\n") else ""
    return lines


def send(s, cmd, wait=1.0):
    s.write((cmd + "\n").encode())
    return read_for(s, wait)


def clk_line(lines):
    for ln in reversed(lines):
        if ln.startswith("CLK "):
            return ln.strip()
    return ""


def parse(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def phase_check():
    s = open_port()
    read_for(s, 0.4)
    for ln in send(s, "S", 1.2):
        if ln.startswith(("STAT frames", "CLK ", "IRQ ")):
            print(ln.strip()[:200])
    s.close()


def phase_anchor():
    now = datetime.datetime.now()
    epoch = int(now.timestamp()) + 1
    s = open_port()
    read_for(s, 0.3)
    for ln in send(s, "t %d 480" % epoch, 0.8):
        if ln.startswith(("ACK t", "NAK t")):
            print(ln.strip())
    worst = 0
    t0 = time.time()
    while time.time() - t0 < 60:
        read_for(s, 4.2)
        c = clk_line(send(s, "S", 1.0))
        if c:
            d = parse(c)
            try:
                worst = max(worst, abs(int(d.get("dev", "0"))))
            except ValueError:
                pass
            print("%s  host=%s" % (c, datetime.datetime.now().strftime("%H:%M:%S")),
                  flush=True)
    print("worst |dev| over the run: %d s" % worst)
    s.close()


def phase_load():
    s = open_port()
    read_for(s, 0.4)
    t0 = time.time()
    last_key = 0
    worst = 0
    while time.time() - t0 < 60:
        if time.time() - last_key > 3:
            s.write(b"K 1 6\n")
            last_key = time.time()
        read_for(s, 1.0)
        c = clk_line(send(s, "S", 1.0))
        if c:
            d = parse(c)
            try:
                worst = max(worst, abs(int(d.get("dev", "0"))))
            except ValueError:
                pass
            print("%s  host=%s" % (c, datetime.datetime.now().strftime("%H:%M:%S")),
                  flush=True)
    print("worst |dev| over the run: %d s" % worst)
    s.close()


def phase_snap():
    s = open_port()
    read_for(s, 0.4)
    for ln in send(s, "D", 2.0):
        if ln.startswith(("ACK D", "NAK")):
            print(ln.strip())
    s.close()


def phase_u():
    s = open_port()
    read_for(s, 0.4)
    for ln in send(s, "U", 2.0):
        if ln.startswith(("ACK U", "NAK U")):
            print(ln.strip())
    for ln in send(s, "S", 1.2):
        if ln.startswith(("STAT frames", "CLK ")):
            print(ln.strip()[:200])
    s.close()


if __name__ == "__main__":
    phase = sys.argv[1] if len(sys.argv) > 1 else "check"
    table = {"check": phase_check, "anchor": phase_anchor, "load": phase_load,
             "snap": phase_snap, "u": phase_u}
    if phase not in table:
        sys.stderr.write(__doc__)
        raise SystemExit(2)
    table[phase]()
