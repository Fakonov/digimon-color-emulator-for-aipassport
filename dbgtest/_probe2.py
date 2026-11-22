import serial, time, sys
print("opening", flush=True)
sp = serial.Serial("COM4", 115200, timeout=0.3)
print("opened", flush=True)
sp.write(b"V\n")
print("wrote", flush=True)
time.sleep(1.0)
data = sp.read(16384)
print("read %d bytes" % len(data), flush=True)
sp.close()
for line in data.decode("utf-8", "replace").splitlines():
    if "VER" in line or "NAK" in line or "ACK" in line:
        print("  " + line.strip())
