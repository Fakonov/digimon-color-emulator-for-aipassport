import serial, time
sp = serial.Serial("COM4", 115200, timeout=0.2)
time.sleep(0.3)
sp.reset_input_buffer()
sp.write(b"V\n")
time.sleep(0.8)
buf = b""
deadline = time.time() + 1.0
while time.time() < deadline:
    n = sp.in_waiting
    if n:
        buf += sp.read(n)
    else:
        time.sleep(0.02)
sp.close()
text = buf.decode("utf-8", "replace")
print("bytes=%d" % len(text))
for line in text.splitlines():
    if "VER" in line or "ACK" in line or "NAK" in line:
        print("  " + line.strip())
print("tail:", text[-260:].replace("\n", " | "))
