#!/usr/bin/env python3
"""Set the handset's clock from the PC.

The board has no RTC chip: after a power cut the only time it knows is the epoch
stored with the last machine snapshot, i.e. "the time it was when we last saved".
This script hands it the real time over the console that is already connected:

    python tools/c3_setclock.py [COM4] [tz_minutes]

`tz_minutes` is the local offset east of UTC (default 480 = UTC+8).  The firmware
stores both in NVS, so the next boot resumes from the correct time instead of
from the build timestamp (docs/AUDIT_SAVE_CLOCK_2026-11-24_R3.md).
"""
import datetime
import re
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover
    sys.stderr.write("pyserial missing: run this with the ESP-IDF python\n")
    raise SystemExit(2)

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
tz = int(sys.argv[2]) if len(sys.argv) > 2 else 480

sp = serial.Serial()
sp.port = port
sp.baudrate = 115200
sp.timeout = 0.2
# Keep DTR/RTS low so opening the port does not reset the board.
sp.dtr = False
sp.rts = False
sp.open()
time.sleep(0.3)
sp.reset_input_buffer()

now = datetime.datetime.now()
epoch = int(now.timestamp())
sp.write(("t %d %d\n" % (epoch, tz)).encode())
sp.flush()


def read_until(pattern, seconds):
    deadline = time.time() + seconds
    buf = ""
    while time.time() < deadline:
        chunk = sp.read(4096)
        if chunk:
            buf += chunk.decode("utf-8", "replace")
            if re.search(pattern, buf):
                break
    return buf


ack = read_until(r"^(ACK t|NAK t)", 2.0)
sp.write(b"S\n")
stat = read_until(r"^CLK ", 2.0)
sp.close()

for line in (ack + stat).splitlines():
    if line.startswith(("ACK t", "NAK t", "CLK ")):
        print(line.strip())

m = re.search(r"ACK t (\d\d):(\d\d):(\d\d) src=(\w+)", ack, re.M)
if not m:
    sys.stderr.write("no ACK from the handset: is the console free and the firmware v1.10+\n")
    raise SystemExit(1)

guest = "%s:%s:%s" % (m.group(1), m.group(2), m.group(3))
host = now.strftime("%H:%M:%S")
print("host   ", host)
print("handset", guest, " source", m.group(4))
print("offset ", abs((int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))) -
                    (now.hour * 3600 + now.minute * 60 + now.second)), "s")
