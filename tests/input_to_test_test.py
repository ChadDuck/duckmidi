#!/usr/bin/env python3
"""Tests for tools/input-to-test.py."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONVERTER = ROOT / "tools/input-to-test.py"


class InputToTestTests(unittest.TestCase):
    def _run_converter(self, lines):
        recording = "\n".join(lines) + "\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "recording.txt"
            path.write_text(recording, encoding="utf-8")
            result = subprocess.run(
                [sys.executable, "-I", "-B", str(CONVERTER), str(path)],
                capture_output=True,
                text=True,
                check=True,
            )
        return result.stdout

    def test_converts_events_to_test_ui_calls(self):
        out = self._run_converter(
            [
                "duckmidi-input 1 1400 900",
                "0 motion 120 240",
                "1 button_down 1 120 240",
                "2 button_up 1 120 240",
                "3 key_down 4 64 0",
                "4 key_up 4 0 0",
                "5 text hello world",
                "6 wheel 0 1 120 240",
                "7 key_down 999 0 0",
            ]
        )

        self.assertIn("    // Window size: 1400x900", out)
        self.assertIn("frame(e, 120, 240, false, false, false, false);", out)
        self.assertIn("ImGui::GetIO().AddMousePosEvent(120, 240);", out)
        self.assertIn("ImGui::GetIO().AddMouseButtonEvent(0, true);", out)
        self.assertIn("frame(e, 120, 240, true, false, false, false);", out)
        self.assertIn("ImGui::GetIO().AddMouseButtonEvent(0, false);", out)
        self.assertIn("ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);", out)
        self.assertIn("ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);", out)
        self.assertIn("ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);", out)
        self.assertIn("ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);", out)
        self.assertIn('ImGui::GetIO().AddInputCharactersUTF8("hello world");', out)
        self.assertIn("ImGui::GetIO().AddMouseWheelEvent(0, 1);", out)
        self.assertIn("/* unknown scancode 999 */", out)
        self.assertNotIn("key(e,", out)

        ctrl_true = "ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);"
        key_a_true = "ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);"
        self.assertLess(out.index(ctrl_true), out.index(key_a_true))

        wheel_pos = "ImGui::GetIO().AddMousePosEvent(120, 240);"
        wheel_ev = "ImGui::GetIO().AddMouseWheelEvent(0, 1);"
        text_ev = 'ImGui::GetIO().AddInputCharactersUTF8("hello world");'
        self.assertLess(out.index(text_ev), out.index(wheel_pos, out.index(text_ev)))
        self.assertLess(out.index(wheel_pos, out.index(text_ev)), out.index(wheel_ev))

    def test_shift_held_on_key(self):
        out = self._run_converter(
            [
                "duckmidi-input 1 1400 900",
                "3 key_down 225 1 0",
                "3 key_down 4 1 0",
            ]
        )

        shift_true = "ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);"
        key_a_true = "ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);"
        frame_call = "frame(e, -100, -100, false, false, false, true);"
        self.assertIn(shift_true, out)
        self.assertIn(key_a_true, out)
        self.assertIn(frame_call, out)
        self.assertLess(out.index(shift_true), out.index(key_a_true))
        self.assertLess(out.index(key_a_true), out.index(frame_call))

    def test_left_button_press_and_release_in_same_frame(self):
        out = self._run_converter(
            [
                "duckmidi-input 1 1400 900",
                "1 button_down 1 50 60",
                "1 button_up 1 50 60",
            ]
        )

        btn_down = "ImGui::GetIO().AddMouseButtonEvent(0, true);"
        btn_up = "ImGui::GetIO().AddMouseButtonEvent(0, false);"
        frame_call = "frame(e, 50, 60, false, false, false, false);"
        self.assertIn(btn_down, out)
        self.assertIn(btn_up, out)
        self.assertIn(frame_call, out)
        self.assertLess(out.index(btn_down), out.index(btn_up))
        self.assertLess(out.index(btn_up), out.index(frame_call))

    def test_middle_button(self):
        out = self._run_converter(
            [
                "duckmidi-input 1 1400 900",
                "1 button_down 2 80 90",
            ]
        )

        pos_ev = "ImGui::GetIO().AddMousePosEvent(80, 90);"
        mid_ev = "ImGui::GetIO().AddMouseButtonEvent(2, true);"
        frame_call = "frame(e, 80, 90, false, false, false, false);"
        self.assertIn(pos_ev, out)
        self.assertIn(mid_ev, out)
        self.assertIn(frame_call, out)
        self.assertLess(out.index(pos_ev), out.index(mid_ev))
        self.assertLess(out.index(mid_ev), out.index(frame_call))

    def test_key_down_repeat_is_skipped(self):
        out = self._run_converter(
            [
                "duckmidi-input 1 1400 900",
                "1 key_down 4 0 0",
                "2 key_down 4 0 1",
                "3 key_down 5 0 1",
            ]
        )

        self.assertIn("ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);", out)
        self.assertEqual(out.count("ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);"), 1)
        self.assertNotIn("ImGuiKey_B", out)


if __name__ == "__main__":
    unittest.main()
