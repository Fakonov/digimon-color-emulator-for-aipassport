"""Round-5 bench probe: send console commands, capture the log, print STAT rows.

Usage:
  python tools/r5_probe.py COM4 out.log 10 "J 1" "S"
The command list is sent one per 150 ms before the capture window starts, so a
run can arm a STAT period and inject keys without a second process.
"""
import sys
import time
import os

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
out = sys.argv[2] if len(sys.argv) > 2 else "dbgtest/r5_probe.log"
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
cmds = sys.argv[4:]
# Opening the port pulses the C3's USB-Serial/JTAG reset, so every run starts a
# fresh boot; R5_BOOT_WAIT gives the guest time to reach the clock screen before
# the command list goes out (measured: ~8 s).
boot_wait = float(os.environ.get("R5_BOOT_WAIT", "0"))

# dsrdtr/rtscts off and both lines deasserted: opening the port must not pulse
# the C3's reset (a reset also throws away runtime knobs such as `F 180`).
p = serial.Serial(port, 115200, timeout=0.2, write_timeout=1.0, dsrdtr=False, rtscts=False)
p.dtr = False
p.rts = False
p.reset_input_buffer()
time.sleep(0.15)
if boot_wait > 0:
    time.sleep(boot_wait)
    p.reset_input_buffer()
for c in cmds:
    p.write((c + "\n").encode())
    time.sleep(0.15)

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
print("captured %d bytes in %.1fs -> %s" % (len(buf), seconds, out))
for line in text.splitlines():
    s = line.strip()
    if "STAT" in s or "guest_fps" in s or s.startswith("ACK") or s.startswith("NAK"):
        print(s)
