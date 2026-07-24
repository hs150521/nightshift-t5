"""Decode T5-Link frames from a selected or automatically sniffed port."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent / "tools"))
from t5_link import Frame, ProtocolError, sniff_protocol_port


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port")
    parser.add_argument("--auto", action="store_true")
    args = parser.parse_args()
    port = args.port
    if args.auto or not port:
        port = sniff_protocol_port(2.0)
    if not port:
        raise SystemExit("No T5-Link port detected; pass --port COMx")

    buffer = bytearray()
    print(f"Decoding {port} at 460800 baud; Ctrl+C to stop")
    try:
        with serial.Serial(port, 460800, timeout=0.2) as stream:
            while True:
                buffer.extend(stream.read(2048))
                while 0 in buffer:
                    index = buffer.index(0)
                    packet = bytes(buffer[: index + 1])
                    del buffer[: index + 1]
                    if len(packet) == 1:
                        continue
                    try:
                        frame = Frame.decode(packet)
                        print(
                            f"seq={frame.sequence:5} flags=0x{frame.flags:02x} "
                            f"cmd=0x{frame.command:04x} len={len(frame.payload):4} "
                            f"payload={frame.payload.hex()}"
                        )
                    except ProtocolError as error:
                        print(f"invalid frame ({error}): {packet.hex()}")
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
