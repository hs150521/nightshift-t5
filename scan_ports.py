"""Quick scan of all COM ports at multiple baud rates."""
import serial
import time

ports = ["COM5", "COM6", "COM7"]
bauds = [115200, 460800]

for port in ports:
    for baud in bauds:
        try:
            s = serial.Serial(port, baud, timeout=2)
            data = s.read(1024)
            s.close()
            if data:
                text = data.decode("utf-8", "replace")
                print(f"=== {port} @ {baud} === {len(data)} bytes ===")
                print(text[:500])
            else:
                print(f"=== {port} @ {baud} === (nothing)")
        except Exception as e:
            print(f"=== {port} @ {baud} === ERROR: {e}")
