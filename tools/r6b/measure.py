"""Scheduled serial probe: send console commands with delays, capture everything.

Usage: python tools/r6b/measure.py COM4 out.log "0.0:J 1,3.0:C,3.2:K 10 6,6.0:C,6.2:R,7.2:R" 12
Each item is DELAY:COMMAND; DELAY is counted from the previous item.
"""
import sys
import time

import serial

port = sys.argv[1]
out = sys.argv[2]
spec = sys.argv[3]
seconds = float(sys.argv[4]) if len(sys.argv) > 4 else 10.0

p = serial.Serial(port, 115200, timeout=0.1, write_timeout=1.0, dsrdtr=False, rtscts=False)
p.dtr = False
p.rts = False
p.reset_input_buffer()
time.sleep(0.2)

items = []
for part in spec.split(","):
    part = part.strip()
    if not part:
        continue
    delay, cmd = part.split(":", 1)
    items.append((float(delay), cmd))

buf = bytearray()
start = time.time()
idx = 0
next_at = start + (items[0][0] if items else 0.0)
while time.time() - start < seconds:
    now = time.time()
    while idx < len(items) and now >= next_at:
        p.write((items[idx][1] + "\n").encode())
        idx += 1
        if idx < len(items):
            next_at = now + items[idx][0]
    d = p.read(8192)
    if d:
        buf += d
p.close()
with open(out, "wb") as fp:
    fp.write(bytes(buf))
text = buf.decode("utf-8", "replace")
print("captured %d bytes in %.1fs -> %s" % (len(buf), seconds, out))
for line in text.splitlines():
    s = line.strip()
    if "LONGF" in s or "guest_fps" in s or s.startswith("CRC") or "win=" in s:
        print(s)
