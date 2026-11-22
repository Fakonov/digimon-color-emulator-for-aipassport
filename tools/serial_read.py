#!/usr/bin/env python3
"""Read raw bytes from a serial port for a fixed number of seconds."""
import sys
import time

import serial


def main() -> int:
    if len(sys.argv) < 4:
        print("usage: serial_read.py PORT BAUD SECONDS", file=sys.stderr)
        return 2
    port, baud, seconds = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
    ser = serial.Serial(port, baud, timeout=0.1)
    deadline = time.time() + seconds
    total = 0
    try:
        while time.time() < deadline:
            data = ser.read(4096)
            if data:
                total += len(data)
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
    finally:
        ser.close()
    print(f"\n[SERIAL_READ] {total} bytes from {port}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
