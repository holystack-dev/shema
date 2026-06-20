#!/usr/bin/env python3
"""Read the ESP32-S3 USB-CDC serial log for a fixed time, then exit.

Usage: read_serial.py [PORT] [SECONDS]
Used for autonomous log capture after flashing (idf.py monitor is interactive).
Tolerates the USB-Serial/JTAG port disappearing/reappearing across a reset.
"""
import sys
import time

try:
    import serial  # pyserial (bundled with the IDF python env)
except ImportError:
    sys.exit("pyserial not available")

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1101"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0


def open_port():
    for _ in range(40):  # ~8s of retries while the CDC port re-enumerates
        try:
            return serial.Serial(PORT, 115200, timeout=0.2)
        except Exception:
            time.sleep(0.2)
    return None


def main():
    ser = open_port()
    if ser is None:
        sys.exit(f"could not open {PORT}")
    deadline = time.time() + SECS
    buf = b""
    while time.time() < deadline:
        try:
            data = ser.read(4096)
        except Exception:
            ser.close()
            ser = open_port()
            if ser is None:
                break
            continue
        if data:
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                print(line.decode("utf-8", "replace"))
                sys.stdout.flush()
    if buf:
        print(buf.decode("utf-8", "replace"))
    try:
        ser.close()
    except Exception:
        pass


if __name__ == "__main__":
    main()
