"""Host-side T5-Link v1 codec and deterministic state simulator."""

from __future__ import annotations

import dataclasses
import struct
import time
from collections import deque

MAGIC = b"T5"
VERSION = 1
MAX_PAYLOAD = 1024
ACK_REQ = 0x01
RESPONSE = 0x02
EVENT = 0x04
VALID_FLAGS = 0x1F


class ProtocolError(ValueError):
    pass


def crc16(data: bytes) -> int:
    value = 0xFFFF
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = ((value << 1) ^ 0x1021) & 0xFFFF if value & 0x8000 else (value << 1) & 0xFFFF
    return value


def cobs_encode(data: bytes) -> bytes:
    output = bytearray(b"\x00")
    code_index = 0
    code = 1
    for byte in data:
        if byte == 0:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(byte)
            code += 1
            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1
    output[code_index] = code
    return bytes(output)


def cobs_decode(data: bytes) -> bytes:
    if not data:
        raise ProtocolError("empty COBS packet")
    output = bytearray()
    index = 0
    while index < len(data):
        code = data[index]
        if code == 0:
            raise ProtocolError("zero COBS code")
        index += 1
        end = index + code - 1
        if end > len(data):
            raise ProtocolError("truncated COBS block")
        output.extend(data[index:end])
        index = end
        if code != 0xFF and index < len(data):
            output.append(0)
    return bytes(output)


@dataclasses.dataclass(frozen=True)
class Frame:
    flags: int
    sequence: int
    command: int
    payload: bytes = b""

    def encode(self) -> bytes:
        if len(self.payload) > MAX_PAYLOAD:
            raise ProtocolError("payload too large")
        if self.flags & ~VALID_FLAGS or self.flags & RESPONSE and self.flags & EVENT:
            raise ProtocolError("invalid flags")
        raw = struct.pack(
            "<2sBBHHH", MAGIC, VERSION, self.flags,
            self.sequence, self.command, len(self.payload)
        ) + self.payload
        raw += struct.pack("<H", crc16(raw))
        return cobs_encode(raw) + b"\x00"

    @classmethod
    def decode(cls, wire: bytes) -> "Frame":
        if not wire.endswith(b"\x00"):
            raise ProtocolError("missing delimiter")
        raw = cobs_decode(wire[:-1])
        if len(raw) < 12:
            raise ProtocolError("short frame")
        magic, version, flags, sequence, command, length = struct.unpack("<2sBBHHH", raw[:10])
        if magic != MAGIC:
            raise ProtocolError("bad magic")
        if version != VERSION:
            raise ProtocolError("unsupported version")
        if flags & ~VALID_FLAGS or flags & RESPONSE and flags & EVENT:
            raise ProtocolError("invalid flags")
        if length > MAX_PAYLOAD or len(raw) != 12 + length:
            raise ProtocolError("length mismatch")
        if crc16(raw[:-2]) != struct.unpack("<H", raw[-2:])[0]:
            raise ProtocolError("bad CRC")
        return cls(flags, sequence, command, raw[10:-2])


def encode_string(text: str) -> bytes:
    encoded = text.encode("utf-8")
    return struct.pack("<H", len(encoded)) + encoded


def encode_ui_action(action: int, object_type: int, object_id: int,
                     value: int = 0, text: str = "") -> bytes:
    return struct.pack("<HBIi", action, object_type, object_id, value) + encode_string(text)


class StreamAccumulator:
    """Delimiter accumulator with drop-until-delimiter overflow recovery."""

    def __init__(self, maximum: int) -> None:
        self.maximum = maximum
        self.buffer = bytearray()
        self.dropping = False

    def feed(self, data: bytes) -> list[bytes]:
        packets = []
        for byte in data:
            if byte == 0:
                if self.dropping:
                    self.dropping = False
                    self.buffer.clear()
                elif self.buffer:
                    packets.append(bytes(self.buffer) + b"\x00")
                    self.buffer.clear()
            elif not self.dropping:
                if len(self.buffer) >= self.maximum:
                    self.buffer.clear()
                    self.dropping = True
                else:
                    self.buffer.append(byte)
        return packets


class RequestCache:
    def __init__(self, capacity: int = 32) -> None:
        self._entries: deque[tuple[tuple[int, int], bytes]] = deque(maxlen=capacity)

    def run(self, sequence: int, command: int, operation) -> bytes:
        key = sequence, command
        for cached_key, result in self._entries:
            if cached_key == key:
                return result
        result = operation()
        self._entries.append((key, result))
        return result


class StateSimulator:
    """Models the C store's externally observable transaction rules."""

    REQUIRED = {"mode", "attention", "work"}

    def __init__(self) -> None:
        self.committed = {
            "revision": 0, "mode": 0, "attention": 0,
            "work": 0, "tasks": [], "online": False,
        }
        self.staging: dict | None = None
        self.target = 0
        self.components: set[str] = set()
        self.task_txn: dict | None = None
        self.last_activity_ms = 0

    def begin(self, revision: int) -> None:
        if self.staging is not None:
            raise ProtocolError("sync busy")
        self.staging = {
            key: list(value) if isinstance(value, list) else value
            for key, value in self.committed.items()
        }
        self.staging["revision"] = revision
        self.target = revision
        self.components.clear()

    def set(self, component: str, value, revision: int | None = None) -> None:
        target = self.staging if self.staging is not None else self.committed
        if revision is not None:
            expected = self.target if self.staging is not None else self.committed["revision"]
            if self.staging is not None and revision != expected:
                raise ProtocolError("sync revision conflict")
            if self.staging is None and revision < expected:
                raise ProtocolError("stale revision")
            target["revision"] = revision
        target[component] = value
        if self.staging is not None:
            self.components.add(component)

    def end(self, revision: int) -> None:
        if self.staging is None or revision != self.target:
            raise ProtocolError("sync conflict")
        if self.task_txn is not None or not self.REQUIRED <= self.components:
            self.staging = None
            raise ProtocolError("incomplete sync")
        online = self.committed["online"]
        self.committed = self.staging
        self.committed["online"] = online
        self.staging = None

    def task_begin(self, revision: int, count: int, maximum: int = 12) -> None:
        if count > maximum:
            raise ProtocolError("task list too large")
        self.task_txn = {"revision": revision, "expected": count, "items": []}

    def task_item(self, revision: int, item: dict) -> None:
        if self.task_txn is None or revision != self.task_txn["revision"]:
            raise ProtocolError("task transaction conflict")
        if len(self.task_txn["items"]) >= self.task_txn["expected"]:
            raise ProtocolError("too many tasks")
        self.task_txn["items"].append(item)

    def task_end(self, revision: int) -> None:
        txn = self.task_txn
        self.task_txn = None
        if txn is None or revision != txn["revision"] or len(txn["items"]) != txn["expected"]:
            raise ProtocolError("incomplete task list")
        target = self.staging if self.staging is not None else self.committed
        target["tasks"] = txn["items"]

    def communication(self, now_ms: int) -> None:
        self.last_activity_ms = now_ms
        self.committed["online"] = True

    def watchdog(self, now_ms: int, timeout_ms: int = 6000) -> None:
        if self.last_activity_ms and now_ms - self.last_activity_ms >= timeout_ms:
            self.committed["online"] = False
            self.staging = None


def discover_serial_ports() -> list[dict[str, str]]:
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise RuntimeError("pyserial is required for serial discovery") from exc
    return [
        {"device": port.device, "description": port.description or "", "hwid": port.hwid or ""}
        for port in list_ports.comports()
    ]


def sniff_protocol_port(seconds: float = 2.0) -> str | None:
    """Return the port producing the most valid T5-Link frames."""
    import serial

    best: tuple[int, str] | None = None
    for info in discover_serial_ports():
        if "BTHENUM" in info["hwid"]:
            continue
        valid = 0
        buffer = bytearray()
        try:
            with serial.Serial(info["device"], 460800, timeout=0.05) as stream:
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    buffer.extend(stream.read(512))
                    while 0 in buffer:
                        index = buffer.index(0)
                        packet = bytes(buffer[: index + 1])
                        del buffer[: index + 1]
                        try:
                            Frame.decode(packet)
                            valid += 1
                        except ProtocolError:
                            pass
        except serial.SerialException:
            continue
        candidate = valid, info["device"]
        if best is None or candidate > best:
            best = candidate
    return best[1] if best and best[0] else None
