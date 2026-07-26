"""Checks for the show-floor standby media and generated LVGL asset."""

from __future__ import annotations

import importlib.util
import struct
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class MediaAssetTests(unittest.TestCase):
    def test_standby_gif_is_panel_sized_and_generated_source_is_current(self) -> None:
        gif_path = ROOT / "media" / "standby.gif"
        data = gif_path.read_bytes()
        self.assertIn(data[:6], (b"GIF87a", b"GIF89a"))
        self.assertEqual(struct.unpack("<HH", data[6:10]), (480, 320))
        self.assertIn(b"NETSCAPE2.0", data)

        module_path = ROOT / "tools" / "gif_to_lvgl_c.py"
        spec = importlib.util.spec_from_file_location("gif_to_lvgl_c", module_path)
        assert spec and spec.loader
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        expected = module.render(data, "nightshift_idle_gif", "standby.gif")
        actual = (ROOT / "src" / "nightshift_idle_gif.c").read_text(
            encoding="utf-8"
        )
        self.assertEqual(actual, expected)

    def test_source_clips_have_no_audio_track(self) -> None:
        for name in ("standby.mp4", "sedentary-reminder.mp4"):
            with self.subTest(name=name):
                data = (ROOT / "media" / "source" / name).read_bytes()
                self.assertIn(b"vide", data)
                self.assertNotIn(b"soun", data)


if __name__ == "__main__":
    unittest.main()
