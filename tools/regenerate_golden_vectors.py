"""Regenerate all canonical T5-Link v1 wire vectors.

The layouts are frozen in contracts/uart/commands.yaml. This generator is
shared verbatim with the Orange Pi repository; neither endpoint's live parser
is treated as the source of truth.
"""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"T5"
VERSION = 1
ACK_REQ = 0x01
RESPONSE = 0x02
EVENT = 0x04


def crc16(data: bytes) -> int:
    value = 0xFFFF
    for byte in data:
        value ^= byte << 8
        for _ in range(8):
            value = (
                ((value << 1) ^ 0x1021) & 0xFFFF
                if value & 0x8000
                else (value << 1) & 0xFFFF
            )
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


@dataclass(frozen=True)
class Frame:
    flags: int
    sequence: int
    command: int
    payload: bytes = b""

    def encode(self) -> bytes:
        raw = (
            struct.pack(
                "<2sBBHHH", MAGIC, VERSION, self.flags,
                self.sequence, self.command, len(self.payload),
            )
            + self.payload
        )
        raw += struct.pack("<H", crc16(raw))
        return cobs_encode(raw) + b"\x00"


def encode_string(text: str) -> bytes:
    encoded = text.encode("utf-8")
    return struct.pack("<H", len(encoded)) + encoded


def encode_ui_action(action: int, object_type: int, object_id: int,
                     value: int = 0, text: str = "") -> bytes:
    return (
        struct.pack("<HBIi", action, object_type, object_id, value)
        + encode_string(text)
    )

COMMANDS = {
    "HELLO": 0x0001,
    "HEARTBEAT": 0x0002,
    "GET_INFO": 0x0003,
    "TIME_SYNC": 0x0004,
    "STATE_SYNC_BEGIN": 0x0010,
    "STATE_SYNC_END": 0x0011,
    "MODE_SET": 0x1001,
    "ATTENTION_SET": 0x1002,
    "WORK_STATE_SET": 0x1003,
    "DASHBOARD_SET": 0x1004,
    "NOTICE_SHOW": 0x1005,
    "TASK_LIST_BEGIN": 0x1101,
    "TASK_ITEM": 0x1102,
    "TASK_LIST_END": 0x1103,
    "UI_ACTION": 0x2001,
    "PAGE_EVENT": 0x2002,
    "LED_OVERRIDE": 0x3001,
    "BACKLIGHT_SET": 0x3002,
}


def _entry(name: str, command_name: str, frame: Frame) -> dict:
    wire = frame.encode()
    return {
        "name": name,
        "sequence": frame.sequence,
        "command": frame.command,
        "command_name": command_name,
        "flags": frame.flags,
        "payload_hex": frame.payload.hex(),
        "raw_hex": wire.hex(),
        "raw_bytes": list(wire),
    }


def build_vectors() -> list[dict]:
    revision = 7
    vectors: list[dict] = []

    def request(name: str, command_name: str, sequence: int,
                payload: bytes = b"") -> None:
        vectors.append(_entry(
            name, command_name,
            Frame(ACK_REQ, sequence, COMMANDS[command_name], payload),
        ))

    request(
        "hello_request", "HELLO", 1,
        struct.pack("<BBBIHH", 1, 1, 0, 0x12345678, 1024, 0x0003)
        + encode_string("nightshift/1.0.0"),
    )
    request("heartbeat_request", "HEARTBEAT", 2,
            struct.pack("<II", 5000, revision))
    vectors.append(_entry(
        "heartbeat_response", "HEARTBEAT",
        Frame(RESPONSE, 2, COMMANDS["HEARTBEAT"],
              struct.pack("<HIII", 0, 6000, revision, 0)),
    ))

    request("get_info_request", "GET_INFO", 3)
    get_info = (
        struct.pack("<HBBHIHHBB", 0, 1, 0, 1024, 0x00000003,
                    480, 320, 16, 12)
        + encode_string("1.0.0")
        + encode_string("TUYA_T5AI_BOARD")
    )
    vectors.append(_entry(
        "get_info_response", "GET_INFO",
        Frame(RESPONSE, 3, COMMANDS["GET_INFO"], get_info),
    ))

    request("time_sync", "TIME_SYNC", 4,
            struct.pack("<Qh", 1_700_000_000_000, 480))
    request("state_sync_begin", "STATE_SYNC_BEGIN", 5,
            struct.pack("<IB", revision, 1))
    request("mode_set_night_exec", "MODE_SET", 6,
            struct.pack("<IBBQ", revision, 2, 2,
                        1_700_000_000_000))
    request("attention_set_need_confirm", "ATTENTION_SET", 7,
            struct.pack("<IIH", revision, 1, 2)
            + encode_string("check"))
    request("work_state_set_running", "WORK_STATE_SET", 8,
            struct.pack("<IBHHIIII", revision, 2, 500, 0,
                        100, 50, 60, 42)
            + encode_string("demo"))
    request("dashboard_set", "DASHBOARD_SET", 9,
            struct.pack("<IHHHHHH", revision, 1, 2, 3, 4, 5, 6))
    request("notice_show", "NOTICE_SHOW", 10,
            struct.pack("<IIBBQ", revision, 44, 1, 1,
                        1_700_000_060_000)
            + encode_string("Warning")
            + encode_string("Pressure input unavailable"))
    request("task_list_begin", "TASK_LIST_BEGIN", 11,
            struct.pack("<IBH", revision, 0, 1))
    request("task_item", "TASK_ITEM", 12,
            struct.pack("<IIBBB", revision, 42, 1, 2, 1)
            + encode_string("demo")
            + encode_string("local"))
    request("task_list_end", "TASK_LIST_END", 13,
            struct.pack("<II", revision, 0))
    request("state_sync_end", "STATE_SYNC_END", 14,
            struct.pack("<II", revision, 0))

    vectors.append(_entry(
        "mode_set_response", "MODE_SET",
        Frame(RESPONSE, 6, COMMANDS["MODE_SET"], struct.pack("<H", 0)),
    ))
    vectors.append(_entry(
        "ui_action_confirm", "UI_ACTION",
        Frame(EVENT | ACK_REQ, 15, COMMANDS["UI_ACTION"],
              encode_ui_action(1, 1, 42, 0, "")),
    ))
    vectors.append(_entry(
        "page_event", "PAGE_EVENT",
        Frame(EVENT, 16, COMMANDS["PAGE_EVENT"],
              struct.pack("<BBI", 1, 3, 4)),
    ))
    request("led_override", "LED_OVERRIDE", 17,
            struct.pack("<BBH", 1, 1, 500))
    request("backlight_set", "BACKLIGHT_SET", 18, b"\x50")
    return vectors


def main() -> None:
    output = (
        Path(__file__).resolve().parents[1]
        / "contracts" / "uart" / "golden_vectors.json"
    )
    vectors = build_vectors()
    output.write_text(
        json.dumps({"golden_vectors": vectors}, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"wrote {len(vectors)} canonical vectors to {output}")


if __name__ == "__main__":
    main()
