from __future__ import annotations

import json
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from t5_link import (  # noqa: E402
    ACK_REQ, EVENT, Frame, ProtocolError, RequestCache, StateSimulator,
    StreamAccumulator,
    cobs_decode, cobs_encode, encode_ui_action,
)


class CodecTests(unittest.TestCase):
    def vectors(self):
        return json.loads((ROOT / "contracts/uart/golden_vectors.json").read_text())["golden_vectors"]

    def test_canonical_golden_vectors(self) -> None:
        for vector in self.vectors():
            with self.subTest(vector=vector["name"]):
                wire = bytes.fromhex(vector["raw_hex"])
                decoded = Frame.decode(wire)
                self.assertEqual(decoded.sequence, vector["sequence"])
                self.assertEqual(decoded.command, vector["command"])
                self.assertEqual(decoded.encode(), wire)

    def test_cobs_boundaries_and_malformed(self) -> None:
        for source in (b"", b"\x00", b"a\x00b", bytes(range(1, 255)), b"x" * 300):
            self.assertEqual(cobs_decode(cobs_encode(source)), source)
        with self.assertRaises(ProtocolError):
            cobs_decode(b"\x04ab")

    def test_crc_length_version_and_flags_failures(self) -> None:
        good = bytearray(Frame(ACK_REQ, 9, 1, b"abc").encode())
        bad_crc = good.copy()
        bad_crc[-2] ^= 1
        with self.assertRaises(ProtocolError):
            Frame.decode(bytes(bad_crc))
        raw = bytearray(cobs_decode(good[:-1]))
        raw[8:10] = struct.pack("<H", 9)
        with self.assertRaises(ProtocolError):
            Frame.decode(cobs_encode(raw) + b"\x00")
        raw = bytearray(cobs_decode(good[:-1]))
        raw[2] = 2
        with self.assertRaises(ProtocolError):
            Frame.decode(cobs_encode(raw) + b"\x00")
        with self.assertRaises(ProtocolError):
            Frame(0x80, 1, 1).encode()
        with self.assertRaises(ProtocolError):
            Frame(EVENT | 0x02, 1, 1).encode()

    def test_mode_set_golden_offsets(self) -> None:
        vector = next(v for v in self.vectors() if v["name"] == "mode_set_night_exec")
        payload = Frame.decode(bytes.fromhex(vector["raw_hex"])).payload
        self.assertEqual(len(payload), 17)
        self.assertEqual(struct.unpack("<IBIQ", payload), (2, 2, 0, 100000))

    def test_work_state_and_heartbeat_lengths(self) -> None:
        work = next(v for v in self.vectors() if v["name"] == "work_state_set_running")
        payload = Frame.decode(bytes.fromhex(work["raw_hex"])).payload
        self.assertEqual(len(payload), 29)
        self.assertEqual(struct.unpack("<BHHIIII", payload[:21]), (2, 500, 0, 100, 50, 60, 7))
        heartbeat = next(v for v in self.vectors() if v["name"] == "heartbeat_response")
        self.assertEqual(len(Frame.decode(bytes.fromhex(heartbeat["raw_hex"])).payload), 14)

    def test_ui_action_payload_layout(self) -> None:
        payload = encode_ui_action(1, 1, 0x12345678, -4, "ok")
        self.assertEqual(struct.unpack("<HBIi", payload[:11]), (1, 1, 0x12345678, -4))
        self.assertEqual(payload[11:], b"\x02\x00ok")

    def test_stream_overflow_recovers_at_next_delimiter(self) -> None:
        stream = StreamAccumulator(32)
        good = Frame(ACK_REQ, 2, 2, b"").encode()
        self.assertEqual(stream.feed(b"x" * 40 + b"\x00" + good), [good])

    def test_notice_layout_strings_are_bounded(self) -> None:
        title = b"Warning"
        body = b"Pressure input unavailable"
        payload = (
            struct.pack("<IIBBQ", 9, 44, 1, 1, 123456)
            + struct.pack("<H", len(title)) + title
            + struct.pack("<H", len(body)) + body
        )
        fixed = struct.unpack("<IIBBQ", payload[:18])
        self.assertEqual(fixed, (9, 44, 1, 1, 123456))
        title_len = struct.unpack("<H", payload[18:20])[0]
        self.assertEqual(payload[20:20 + title_len], title)


class StateTests(unittest.TestCase):
    def test_duplicate_replay(self) -> None:
        cache = RequestCache()
        calls = 0

        def operation() -> bytes:
            nonlocal calls
            calls += 1
            return b"\x00\x00"

        cache.run(4, 0x1001, operation)
        cache.run(4, 0x1001, operation)
        self.assertEqual(calls, 1)

    def test_stale_revision_and_atomic_sync(self) -> None:
        state = StateSimulator()
        state.set("mode", 1, revision=5)
        with self.assertRaises(ProtocolError):
            state.set("attention", 1, revision=4)
        state.begin(6)
        state.set("mode", 2, revision=6)
        with self.assertRaises(ProtocolError):
            state.end(6)
        self.assertEqual(state.committed["mode"], 1)
        state.begin(6)
        state.set("mode", 2, revision=6)
        state.set("attention", 1, revision=6)
        state.set("work", 2)
        state.end(6)
        self.assertEqual((state.committed["revision"], state.committed["mode"]), (6, 2))

    def test_task_list_atomic_replacement(self) -> None:
        state = StateSimulator()
        state.committed["tasks"] = [{"id": 1}]
        state.task_begin(1, 2)
        state.task_item(1, {"id": 2})
        self.assertEqual(state.committed["tasks"], [{"id": 1}])
        with self.assertRaises(ProtocolError):
            state.task_end(1)
        self.assertEqual(state.committed["tasks"], [{"id": 1}])
        state.task_begin(1, 2)
        state.task_item(1, {"id": 2})
        state.task_item(1, {"id": 3})
        state.task_end(1)
        self.assertEqual(state.committed["tasks"], [{"id": 2}, {"id": 3}])

    def test_watchdog_transition_and_recovery(self) -> None:
        state = StateSimulator()
        state.communication(100)
        state.watchdog(6099)
        self.assertTrue(state.committed["online"])
        state.watchdog(6100)
        self.assertFalse(state.committed["online"])
        state.communication(6200)
        self.assertTrue(state.committed["online"])


if __name__ == "__main__":
    unittest.main()
