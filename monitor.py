"""Monitor COM7 at 460800 baud for Nightshift T5 debug output."""
import serial
import time
import sys

PORT = "COM7"
BAUD = 460800

try:
    s = serial.Serial(PORT, BAUD, timeout=0.5)
except serial.SerialException as e:
    print(f"Cannot open {PORT}: {e}")
    sys.exit(1)

print(f"=== {PORT} @ {BAUD} === Press Ctrl+C to quit ===")
sys.stdout.flush()
t0 = time.time()

try:
    while True:
        data = s.read(2048)
        if data:
            print(data.decode("utf-8", "replace"), end="", flush=True)
        else:
            elapsed = time.time() - t0
            if int(elapsed) % 5 == 0 and int(elapsed) > 0:
                print(f"\n[{elapsed:.0f}s] waiting...", flush=True)
                time.sleep(1)
except KeyboardInterrupt:
    pass
finally:
    s.close()
    print("\nPort closed.")
