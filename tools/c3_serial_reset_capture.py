"""Reset the ESP32-C3 over DTR/RTS over then capture its UART log for N seconds."""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
out = sys.argv[2] if len(sys.argv) > 2 else "dbgtest/c3_serial.log"
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0

p = serial.Serial(port, 115200, timeout=0.3)
p.setDTR(False)
p.setRTS(True)
time.sleep(0.15)
p.setRTS(False)
time.sleep(0.1)

t0 = time.time()
buf = bytearray()
while time.time() - t0 < seconds:
    d = p.read(16384)
    if d:
        buf += d
p.close()
with open(out, "wb") as fp:
    fp.write(bytes(buf))
text = bytes(buf)
print("captured %d bytes in %.0fs -> %s (shot blocks: %d)"
      % (len(text), seconds, out, text.count(b"SHOT_BEGIN")))
for line in text.decode("utf-8", "replace").splitlines():
    if "BOOT_OK" in line or "clock seeded" in line:
        print(line.strip())
