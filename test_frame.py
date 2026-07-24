"""Send a valid ATTENTION_SET request to an explicitly selected UART0 port."""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent / "tools"))
from t5_link import ACK_REQ, Frame, ProtocolError, encode_string


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="Verified CH342 UART0 interface")
    args = parser.parse_args()
    payload = struct.pack("<IIH", 1, 1, 1) + encode_string("host smoke test")
    request = Frame(ACK_REQ, 42, 0x1002, payload).encode()
    print("TX", request.hex(" "))
    with serial.Serial(args.port, 460800, timeout=1) as stream:
        stream.reset_input_buffer()
        stream.write(request)
        stream.flush()
        response = stream.read_until(b"\x00")
    try:
        frame = Frame.decode(response)
    except ProtocolError as error:
        print(f"invalid/no response: {error}; bytes={response.hex()}")
        return 1
    print(
        f"RX seq={frame.sequence} flags=0x{frame.flags:02x} "
        f"cmd=0x{frame.command:04x} payload={frame.payload.hex()}"
    )
    return 0 if frame.sequence == 42 and frame.command == 0x1002 else 1


if __name__ == "__main__":
    raise SystemExit(main())
