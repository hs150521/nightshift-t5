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


def decode_panel_hello(frame: Frame) -> dict[str, object]:
    payload = frame.payload
    if frame.flags != ACK_REQ:
        raise AssertionError("panel HELLO must use ACK_REQ only")
    if len(payload) < 13:
        raise AssertionError("panel HELLO payload is too short")
    role, major, minor, boot_id, max_payload, capabilities = struct.unpack(
        "<BBBIHH", payload[:11]
    )
    version_length = struct.unpack("<H", payload[11:13])[0]
    if len(payload) != 13 + version_length:
        raise AssertionError("panel HELLO version length mismatch")
    version = payload[13:].decode("utf-8")
    if (
        role != 2
        or major != 1
        or minor != 0
        or boot_id == 0
        or max_payload != 1024
    ):
        raise AssertionError("panel HELLO contains noncanonical fields")
    return {
        "peer_role": role,
        "protocol_major": major,
        "protocol_minor": minor,
        "boot_id": boot_id,
        "max_payload": max_payload,
        "capabilities": capabilities,
        "software_version": version,
    }


class DeviceHarness:
    def __init__(self, port: str) -> None:
        self.port = port
        self.serial = serial.Serial(port, 460800, timeout=0.05)
        self.sequence = 100
        self.rx = bytearray()
        self.invalid_wires: list[bytes] = []

    def close(self) -> None:
        self.serial.close()

    def reopen(self) -> None:
        """Work around the Windows CH342 bridge losing later host writes."""
        self.serial.close()
        time.sleep(1.0)
        self.serial = serial.Serial(
            self.port, 460800, timeout=0.05
        )
        self.rx.clear()

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
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("T5 response timeout")
            wire = self.read_wire(remaining)
            try:
                return Frame.decode(wire), wire
            except ProtocolError:
                self.invalid_wires.append(wire)
                continue

    def send_ack(self, frame: Frame, status: int = OK) -> None:
        ack = Frame(
            RESPONSE,
            frame.sequence,
            frame.command,
            struct.pack("<H", status),
        )
        self.serial.write(ack.encode())
        self.serial.flush()
        # The board's CH342 bridge needs a short host-write turn-around
        # before a second frame; this does not change either wire frame.
        time.sleep(0.05)

    def wait_panel_hello(
        self, timeout: float = 20.0,
    ) -> tuple[Frame, bytes, dict[str, object]]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            frame, wire = self.read_frame(deadline - time.monotonic())
            if (
                frame.command == HELLO
                and not frame.flags & (RESPONSE | EVENT)
            ):
                return frame, wire, decode_panel_hello(frame)
        raise TimeoutError("panel HELLO timeout")

    def exchange_opi_hello(
        self, boot_id: int, expected_t5_boot_id: int,
    ) -> tuple[Frame, bytes, dict[str, object]]:
        payload = (
            struct.pack("<BBBIHH", 1, 1, 0, boot_id, 1024, 0)
            + encode_string("device-acceptance/1")
        )
        request = Frame(
            ACK_REQ, self._next_sequence(), HELLO, payload
        )
        request_wire = request.encode()
        response: Frame | None = None
        panel_frame: Frame | None = None
        panel_wire = b""
        panel_info: dict[str, object] | None = None
        for _ in range(5):
            self.reopen()
            self.serial.write(request_wire)
            self.serial.flush()
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline and (
                response is None or panel_frame is None
            ):
                try:
                    frame, wire = self.read_frame(
                        deadline - time.monotonic()
                    )
                except TimeoutError:
                    break
                if (
                    frame.flags & RESPONSE
                    and frame.sequence == request.sequence
                    and frame.command == HELLO
                ):
                    response = frame
                elif (
                    frame.command == HELLO
                    and not frame.flags & (RESPONSE | EVENT)
                ):
                    panel_frame = frame
                    panel_wire = wire
                    panel_info = decode_panel_hello(frame)
                    if panel_info["boot_id"] != expected_t5_boot_id:
                        raise AssertionError(
                            "T5 boot ID changed without a T5 reboot"
                        )
                    self.send_ack(frame)
            if response is not None and panel_frame is not None:
                break
        if response is not None and self.status(response) != OK:
            raise AssertionError("OPI HELLO was not ACKed")
        if panel_frame is None or panel_info is None:
            raise AssertionError(
                "new OPI session did not trigger panel HELLO"
            )
        return panel_frame, panel_wire, panel_info

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
        self.reopen()
        self.serial.write(request_wire + request_wire)
        self.serial.flush()
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
    opi_boot_a = (time.time_ns() & 0xFFFFFFFF) | 1
    opi_boot_b = (opi_boot_a ^ 0x5A5A5A5A) | 1
    try:
        harness.serial.reset_input_buffer()

        # Drop the first ACK deliberately and prove the firmware retransmits
        # the exact immutable boot HELLO frame.
        first_hello, first_wire, panel = harness.wait_panel_hello()
        second_hello, second_wire, panel_retry = harness.wait_panel_hello(18.0)
        if first_hello != second_hello or first_wire != second_wire:
            raise AssertionError("panel HELLO retry changed frame bytes")
        if panel != panel_retry:
            raise AssertionError("panel HELLO retry changed decoded fields")
        evidence["panel_hello"] = panel
        evidence["panel_hello"]["exact_retry"] = True
        evidence["panel_hello"]["sequence"] = first_hello.sequence

        panel_after_opi, panel_after_opi_wire, _ = (
            harness.exchange_opi_hello(
                opi_boot_a, int(panel["boot_id"])
            )
        )
        if panel_after_opi_wire != first_wire:
            raise AssertionError(
                "new OPI session did not reuse immutable T5 HELLO"
            )
        evidence["opi_session_a"] = {
            "boot_id": opi_boot_a,
            "panel_hello_sequence": panel_after_opi.sequence,
            "t5_hello_unchanged": True,
        }

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

        # Same OPI session must retain duplicate replay even if a caller
        # incorrectly changes the payload under the same (seq, command).
        reused_sequence = harness._next_sequence()
        first_mode, _, first_response_wire = harness.request(
            MODE_SET,
            struct.pack("<IBBQ", target + 1, 1, 1, 1),
            reused_sequence,
        )
        if harness.status(first_mode) != OK:
            raise AssertionError("first same-session MODE_SET failed")
        _, _, applied_after_first, _ = harness.heartbeat()
        duplicate_mode, _, duplicate_response_wire = harness.request(
            MODE_SET,
            struct.pack("<IBBQ", target + 2, 2, 2, 2),
            reused_sequence,
        )
        _, _, applied_after_duplicate, _ = harness.heartbeat()
        if (
            harness.status(duplicate_mode) != OK
            or duplicate_response_wire != first_response_wire
            or applied_after_first != target + 1
            or applied_after_duplicate != target + 1
        ):
            raise AssertionError(
                "same OPI boot ID did not preserve request dedup"
            )
        evidence["same_session_duplicate_preserved"] = True

        # Simulate restarting only OPI, then reuse the old request sequence.
        _, restarted_panel_wire, _ = harness.exchange_opi_hello(
            opi_boot_b, int(panel["boot_id"])
        )
        if restarted_panel_wire != first_wire:
            raise AssertionError("T5 HELLO changed across OPI restart")
        after_restart, _, _ = harness.request(
            MODE_SET,
            struct.pack("<IBBQ", target + 2, 2, 2, 2),
            reused_sequence,
        )
        if harness.status(after_restart) != OK:
            raise AssertionError("reused sequence after OPI restart failed")
        _, _, applied_after_restart, _ = harness.heartbeat()
        if applied_after_restart != target + 2:
            raise AssertionError(
                "new OPI session replayed an old cached response"
            )
        evidence["opi_restart"] = {
            "boot_id": opi_boot_b,
            "reused_sequence": reused_sequence,
            "new_command_applied_revision": applied_after_restart,
            "t5_boot_id_unchanged": int(panel["boot_id"]),
        }

        harness.full_sync(target + 3)
        evidence["post_session_full_resync_revision"] = target + 3

        stale, _, _ = harness.request(
            MODE_SET,
            struct.pack("<IBBQ", target + 2, 0, 0, 0),
        )
        if harness.status(stale) != STATE_CONFLICT:
            raise AssertionError("stale MODE_SET was not rejected")
        rollback, _, _ = harness.request(
            STATE_SYNC_BEGIN, struct.pack("<IB", target + 2, 0)
        )
        if harness.status(rollback) != STATE_CONFLICT:
            raise AssertionError("rollback STATE_SYNC_BEGIN was not rejected")
        evidence["stale_and_rollback_status"] = STATE_CONFLICT

        same_revision, _, _ = harness.request(
            STATE_SYNC_BEGIN, struct.pack("<IB", target + 3, 0)
        )
        if harness.status(same_revision) != OK:
            raise AssertionError("same-revision explicit resync was rejected")
        # Deliberately incomplete: END must reject and abandon staging.
        incomplete, _, _ = harness.request(
            STATE_SYNC_END, struct.pack("<II", target + 3, 0)
        )
        evidence["incomplete_sync_rejected"] = (
            harness.status(incomplete) != OK
        )
        if not evidence["incomplete_sync_rejected"]:
            raise AssertionError("incomplete sync unexpectedly committed")
        evidence["invalid_uart_packets"] = len(harness.invalid_wires)
        if harness.invalid_wires:
            evidence["invalid_uart_hex"] = [
                wire.hex() for wire in harness.invalid_wires[:4]
            ]
            raise AssertionError(
                "UART0 emitted bytes outside valid framed traffic"
            )
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
