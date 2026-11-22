"""Capture N seconds of the C3 emulator's UART log to a file (COM4 default)."""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
out = sys.argv[2] if len(sys.argv) > 2 else "dbgtest/c3_serial.log"
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0

p = serial.Serial(port, 115200, timeout=0.3)
p.reset_input_buffer()   # drop whatever the CDC buffer kept from a previous run
t0 = time.time()
buf = bytearray()
while time.time() - t0 < seconds:
    d = p.read(8192)
    if d:
        buf += d
p.close()
with open(out, "wb") as fp:
    fp.write(bytes(buf))
text = buf.decode("utf-8", "replace")
print("captured %d bytes in %.0fs -> %s" % (len(buf), seconds, out))
for line in text.splitlines():
    if "BOOT_OK" in line or "clock seeded" in line:
        print(line.strip())
lines = [l for l in text.splitlines() if "DIGICORE" in l]
if lines:
    print(lines[-1].strip())
