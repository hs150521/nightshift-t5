"""Real-device canonical T5-Link acceptance harness.

Examples:
  python tools/device_acceptance.py --port COM7
  python tools/device_acceptance.py --port COM7 --listen-action 3
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from t5_link import ACK_REQ, EVENT, RESPONSE, Frame, ProtocolError, encode_string

HELLO = 0x0001
HEARTBEAT = 0x0002
GET_INFO = 0x0003
STATE_SYNC_BEGIN = 0x0010
STATE_SYNC_END = 0x0011
MODE_SET = 0x1001
ATTENTION_SET = 0x1002
WORK_STATE_SET = 0x1003
DASHBOARD_SET = 0x1004
NOTICE_SHOW = 0x1005
TASK_LIST_BEGIN = 0x1101
TASK_ITEM = 0x1102
TASK_LIST_END = 0x1103
UI_ACTION = 0x2001
BACKLIGHT_SET = 0x3002

OK = 0
STATE_CONFLICT = 11


class DeviceHarness:
    def __init__(self, port: str) -> None:
        self.serial = serial.Serial(port, 460800, timeout=0.05)
        self.sequence = 100
        self.rx = bytearray()

    def close(self) -> None:
        self.serial.close()

    def _next_sequence(self) -> int:
        self.sequence += 1
        return self.sequence

    def read_wire(self, timeout: float = 1.0) -> bytes:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.rx.extend(self.serial.read(512))
            if 0 in self.rx:
                end = self.rx.index(0) + 1
                wire = bytes(self.rx[:end])
                del self.rx[:end]
                return wire
        raise TimeoutError("T5 response timeout")

    def read_frame(self, timeout: float = 1.0) -> tuple[Frame, bytes]:
        while True:
            wire = self.read_wire(timeout)
            try:
                return Frame.decode(wire), wire
            except ProtocolError:
                continue

    def request(
        self,
        command: int,
        payload: bytes = b"",
        sequence: int | None = None,
    ) -> tuple[Frame, bytes, bytes]:
        frame = Frame(
            ACK_REQ,
            sequence if sequence is not None else self._next_sequence(),
            command,
            payload,
        )
        request_wire = frame.encode()
        self.serial.write(request_wire)
        while True:
            response, response_wire = self.read_frame()
            if (
                response.flags & RESPONSE
                and response.sequence == frame.sequence
                and response.command == command
            ):
                return response, request_wire, response_wire

    @staticmethod
    def status(frame: Frame) -> int:
        if len(frame.payload) < 2:
            raise AssertionError("response status missing")
        return struct.unpack("<H", frame.payload[:2])[0]

    def expect_ok(self, command: int, payload: bytes = b"") -> Frame:
        response, _, _ = self.request(command, payload)
        status = self.status(response)
        if status != OK:
            raise AssertionError(
                f"command 0x{command:04x} returned status {status}"
            )
        return response

    def heartbeat(self) -> tuple[int, int, int, int]:
        response = self.expect_ok(
            HEARTBEAT,
            struct.pack("<II", int(time.monotonic() * 1000) & 0xFFFFFFFF, 0),
        )
        if len(response.payload) != 14:
            raise AssertionError(
                f"heartbeat response is {len(response.payload)} bytes, not 14"
            )
        return struct.unpack("<HIII", response.payload)

    def full_sync(self, revision: int, work_state: int = 5) -> None:
        self.expect_ok(STATE_SYNC_BEGIN, struct.pack("<IB", revision, 1))
        self.expect_ok(
            MODE_SET,
            struct.pack("<IBBQ", revision, 1, 1,
                        int(time.time() * 1000)),
        )
        self.expect_ok(
            ATTENTION_SET,
            struct.pack("<IIH", revision, 1, 1)
            + encode_string("Confirm task #42"),
        )
        self.expect_ok(
            WORK_STATE_SET,
            struct.pack(
                "<IBHHIIII",
                revision, work_state, 500, 0, 100, 50, 60, 42,
            )
            + encode_string("Canonical device test"),
        )
        self.expect_ok(
            DASHBOARD_SET,
            struct.pack("<IHHHHHH", revision, 1, 2, 1, 0, 5, 1),
        )
        self.expect_ok(
            NOTICE_SHOW,
            struct.pack("<IIBBQ", revision, 44, 1, 1, 0)
            + encode_string("Device test")
            + encode_string("Canonical full sync committed"),
        )
        self.expect_ok(
            TASK_LIST_BEGIN, struct.pack("<IBH", revision, 0, 1)
        )
        self.expect_ok(
            TASK_ITEM,
            struct.pack("<IIBBB", revision, 42, 1, work_state, 1)
            + encode_string("Canonical device test")
            + encode_string("acceptance"),
        )
        self.expect_ok(TASK_LIST_END, struct.pack("<II", revision, 0))
        self.expect_ok(STATE_SYNC_END, struct.pack("<II", revision, 0))


def run_automated(port: str) -> dict:
    harness = DeviceHarness(port)
    evidence: dict[str, object] = {}
    try:
        harness.serial.reset_input_buffer()
        silent_deadline = time.monotonic() + 3.0
        unsolicited = bytearray()
        while time.monotonic() < silent_deadline:
            unsolicited.extend(harness.serial.read(512))
        evidence["unsolicited_uart_bytes"] = len(unsolicited)

        hello = (
            struct.pack("<BBBIHH", 1, 1, 0, 0xA5A5A5A5, 1024, 0)
            + encode_string("device-acceptance/1")
        )
        harness.expect_ok(HELLO, hello)
        status, uptime, applied, errors = harness.heartbeat()
        evidence["heartbeat"] = {
            "status": status,
            "t5_uptime_ms": uptime,
            "applied_revision": applied,
            "error_flags": errors,
            "payload_length": 14,
        }

        target = applied + 1
        harness.full_sync(target)
        evidence["full_sync_revision"] = target

        duplicate_sequence = harness._next_sequence()
        first, request_wire, first_wire = harness.request(
            BACKLIGHT_SET, b"\x50", duplicate_sequence
        )
        second, request_wire_2, second_wire = harness.request(
            BACKLIGHT_SET, b"\x50", duplicate_sequence
        )
        if request_wire != request_wire_2 or first_wire != second_wire:
            raise AssertionError("duplicate response was not replayed exactly")
        if harness.status(first) != OK or harness.status(second) != OK:
            raise AssertionError("duplicate request failed")
        evidence["duplicate_replay_identical"] = True

        stale, _, _ = harness.request(
            MODE_SET,
            struct.pack("<IBBQ", target - 1, 0, 0, 0),
        )
        if harness.status(stale) != STATE_CONFLICT:
            raise AssertionError("stale MODE_SET was not rejected")
        rollback, _, _ = harness.request(
            STATE_SYNC_BEGIN, struct.pack("<IB", target - 1, 0)
        )
        if harness.status(rollback) != STATE_CONFLICT:
            raise AssertionError("rollback STATE_SYNC_BEGIN was not rejected")
        evidence["stale_and_rollback_status"] = STATE_CONFLICT

        same_revision, _, _ = harness.request(
            STATE_SYNC_BEGIN, struct.pack("<IB", target, 0)
        )
        if harness.status(same_revision) != OK:
            raise AssertionError("same-revision explicit resync was rejected")
        # Deliberately incomplete: END must reject and abandon staging.
        incomplete, _, _ = harness.request(
            STATE_SYNC_END, struct.pack("<II", target, 0)
        )
        evidence["incomplete_sync_rejected"] = (
            harness.status(incomplete) != OK
        )
        if not evidence["incomplete_sync_rejected"]:
            raise AssertionError("incomplete sync unexpectedly committed")
        return evidence
    finally:
        harness.close()


def listen_for_retry(port: str, expected_action: int) -> dict:
    harness = DeviceHarness(port)
    try:
        hello = (
            struct.pack("<BBBIHH", 1, 1, 0, 0xB6B6B6B6, 1024, 0)
            + encode_string("touch-acceptance/1")
        )
        harness.expect_ok(HELLO, hello)
        _, _, applied, _ = harness.heartbeat()
        harness.full_sync(applied + 1, work_state=5)
        print(
            f"Touch the requested control now; waiting for UI action "
            f"{expected_action}...",
            flush=True,
        )
        deadline = time.monotonic() + 30
        first_frame: Frame | None = None
        first_wire = b""
        last_heartbeat = time.monotonic()
        while time.monotonic() < deadline:
            if time.monotonic() - last_heartbeat >= 2:
                harness.heartbeat()
                last_heartbeat = time.monotonic()
            try:
                frame, wire = harness.read_frame(timeout=0.1)
            except TimeoutError:
                continue
            if frame.command != UI_ACTION or not frame.flags & EVENT:
                continue
            action, object_type, object_id, value = struct.unpack(
                "<HBIi", frame.payload[:11]
            )
            if action != expected_action:
                continue
            if first_frame is None:
                first_frame = frame
                first_wire = wire
                continue  # Drop the first ACK to force target retransmission.
            if wire != first_wire or frame != first_frame:
                raise AssertionError("UI_ACTION retry changed wire bytes")
            ack = Frame(
                RESPONSE,
                frame.sequence,
                frame.command,
                struct.pack("<H", OK),
            )
            harness.serial.write(ack.encode())
            return {
                "action": action,
                "object_type": object_type,
                "object_id": object_id,
                "value": value,
                "sequence": frame.sequence,
                "identical_retry": True,
            }
        raise TimeoutError("no matching touch UI_ACTION received")
    finally:
        harness.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--listen-action", type=int)
    args = parser.parse_args()
    result = (
        listen_for_retry(args.port, args.listen_action)
        if args.listen_action is not None
        else run_automated(args.port)
    )
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
