"""Enumerate serial devices and sniff for valid T5-Link traffic."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "tools"))
from t5_link import discover_serial_ports, sniff_protocol_port


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sniff-seconds", type=float, default=1.0)
    args = parser.parse_args()
    ports = discover_serial_ports()
    for port in ports:
        print(f"{port['device']:6}  {port['description']}")
        print(f"        {port['hwid']}")
    match = sniff_protocol_port(args.sniff_seconds)
    if match:
        print(f"T5-Link traffic detected on {match}")
        return 0
    print("No valid T5-Link frames observed; select the CH342 interface by descriptor.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
