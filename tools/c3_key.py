"""Inject key commands over the USB-Serial/JTAG port and read the reply.

The board's hardware UART RX is not wired, but the C3 exposes USB-Serial/JTAG,
so `console_exec()` may still be reachable through the same COM port that
carries the log.  This probes that: write "K 1 8" and look for "ACK K".
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
cmd = sys.argv[2] if len(sys.argv) > 2 else "K 1 8"
wait = float(sys.argv[3]) if len(sys.argv) > 3 else 1.5

p = serial.Serial(port, 115200, timeout=0.2, write_timeout=1.0)
p.reset_input_buffer()
time.sleep(0.2)
p.write((cmd + "\n").encode())
t0 = time.time()
buf = bytearray()
while time.time() - t0 < wait:
    d = p.read(4096)
    if d:
        buf += d
p.close()
text = buf.decode("utf-8", "replace")
print("wrote %r, read %d bytes" % (cmd, len(buf)))
for line in text.splitlines():
    if "ACK" in line or "NAK" in line or "STAT" in line:
        print("  " + line.strip())
