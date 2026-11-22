import serial, time
sp = serial.Serial("COM4", 115200, timeout=0.3, write_timeout=0.5)
print("opened", flush=True)
try:
    n = sp.write(b"V\n")
    print("wrote %d" % n, flush=True)
except Exception as e:
    print("write raised: %r" % (e,), flush=True)
time.sleep(1.0)
data = sp.read(16384)
print("read %d bytes" % len(data), flush=True)
sp.close()
hits = [l.strip() for l in data.decode("utf-8", "replace").splitlines() if "VER" in l or "NAK" in l or "ACK" in l]
print("hits:", hits)
