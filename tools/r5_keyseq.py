"""Timed key sequence probe: send console commands at scheduled offsets.

The C3 reboots when the port is opened, so a sequence that has to compare
"first switch" against "second switch" cannot be split across processes.  This
script keeps one port open, starts reading immediately, and fires the command
list at the requested millisecond offsets.

Usage:
  python tools/r5_keyseq.py COM4 out.log 8 14000 "0:J 1" "500:K 2 6" "4000:K 2 6" "8000:K 2 6"

  arg 3 = seconds to wait after opening (device boot, 0 disables)
  arg 4 = total capture time in ms
  args 5.. = "<offset_ms>:<command>"
Prints the STAT lines and any line mentioning a switch/clock/fb event.
"""
import re
import sys
import threading
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
out = sys.argv[2] if len(sys.argv) > 2 else "dbgtest/r5_keyseq.log"
boot_wait = float(sys.argv[3]) if len(sys.argv) > 3 else 8.0
total_ms = float(sys.argv[4]) if len(sys.argv) > 4 else 14000.0
plan = []
for spec in sys.argv[5:]:
    off, _, cmd = spec.partition(":")
    plan.append((float(off), cmd))
plan.sort()

p = serial.Serial(port, 115200, timeout=0.05, write_timeout=1.0, dsrdtr=False, rtscts=False)
p.dtr = False
p.rts = False
p.reset_input_buffer()

buf = bytearray()
stop = threading.Event()


def reader():
    while not stop.is_set():
        try:
            d = p.read(8192)
        except Exception:
            break
        if d:
            buf.extend(d)


th = threading.Thread(target=reader, daemon=True)
th.start()

time.sleep(boot_wait)
t0 = time.monotonic()
sent = 0
while (time.monotonic() - t0) * 1000.0 < total_ms:
    now_ms = (time.monotonic() - t0) * 1000.0
    while sent < len(plan) and plan[sent][0] <= now_ms:
        cmd = plan[sent][1]
        try:
            p.write((cmd + "\n").encode())
        except Exception as exc:
            print("write failed for %r: %s" % (cmd, exc))
        print("t=%5d ms  -> %s" % (int(plan[sent][0]), cmd))
        sent += 1
    time.sleep(0.01)

stop.set()
th.join(timeout=1.0)
p.close()

data = bytes(buf)
with open(out, "wb") as fp:
    fp.write(data)
text = data.decode("utf-8", "replace")
print("captured %d bytes -> %s" % (len(data), out))

keys = ("guest_fps", "ft=", "ACK", "NAK", "clock", "snapshot", "menu", "fb ",
        "screen", "pc=")
for line in text.splitlines():
    s = line.strip()
    if not s:
        continue
    if any(k in s for k in keys):
        # keep the STAT line readable: drop the tail after `fd=`
        m = re.search(r"^I \(\d+\) DIGICORE: (.*)$", s)
        if m and "guest_fps" in s:
            body = m.group(1)
            cut = body.find(" fd=")
            print(body if cut < 0 else body[:cut])
        else:
            print(s)
