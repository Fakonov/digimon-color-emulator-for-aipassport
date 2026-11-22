import sys
import time

import serial

port = sys.argv[1]
out = sys.argv[2]
seconds = float(sys.argv[3])

sp = serial.Serial(port, 115200, timeout=0.2)
# Normal-run reset: keep IO0 (DTR) high, pulse EN (RTS) low.
sp.dtr = False
sp.rts = False
time.sleep(0.1)
sp.rts = True
time.sleep(0.1)
sp.rts = False
time.sleep(0.1)

start = time.time()
with open(out, "w", encoding="utf-8") as f:
    while time.time() - start < seconds:
        data = sp.read(4096)
        if not data:
            continue
        text = data.decode("utf-8", "replace")
        f.write(text)
        f.flush()
        sys.stdout.write(text)
        sys.stdout.flush()
sp.close()
