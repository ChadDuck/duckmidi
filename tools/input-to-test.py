#!/usr/bin/env python3
"""Convert a DEV input recording into a C++ test body for tests/test_ui.h.

Usage:
    python3 tools/input-to-test.py <recording.txt>

Reads a duckmidi-input 1 recording (written by F10 in DEV builds under
<settings>/logs/input-YYYYMMDD-HHMMSS.txt) and prints a C++ function body
using ImGui::GetIO() input events (AddKeyEvent, AddMousePosEvent,
AddMouseButtonEvent, AddMouseWheelEvent, AddInputCharactersUTF8) and frame().
"""

import argparse
from pathlib import Path
import sys

SDL_BUTTON_LEFT = 1
SDL_BUTTON_RIGHT = 3

SDL_TO_IMGUI_BUTTON = {
    1: 0,
    2: 2,
    3: 1,
    4: 3,
    5: 4,
}

SDL_KMOD_SHIFT = 0x0003
SDL_KMOD_CTRL = 0x00C0
SDL_KMOD_ALT = 0x0300

MODIFIER_SCANCODES = {
    224: "ctrl",
    225: "shift",
    226: "alt",
    228: "ctrl",
    229: "shift",
    230: "alt",
}


def build_scancode_table():
    table = {}
    for i in range(26):
        table[4 + i] = f"ImGuiKey_{chr(ord('A') + i)}"
    for i in range(9):
        table[30 + i] = f"ImGuiKey_{i + 1}"
    table[39] = "ImGuiKey_0"
    table.update(
        {
            40: "ImGuiKey_Enter",
            41: "ImGuiKey_Escape",
            42: "ImGuiKey_Backspace",
            43: "ImGuiKey_Tab",
            44: "ImGuiKey_Space",
            74: "ImGuiKey_Home",
            75: "ImGuiKey_PageUp",
            76: "ImGuiKey_Delete",
            77: "ImGuiKey_End",
            78: "ImGuiKey_PageDown",
            79: "ImGuiKey_RightArrow",
            80: "ImGuiKey_LeftArrow",
            81: "ImGuiKey_DownArrow",
            82: "ImGuiKey_UpArrow",
        }
    )
    for i in range(12):
        table[58 + i] = f"ImGuiKey_F{i + 1}"
    return table


SCANCODES = build_scancode_table()


def unescape_text(raw):
    out = []
    i = 0
    while i < len(raw):
        if raw[i] == "\\" and i + 1 < len(raw):
            i += 1
            ch = raw[i]
            if ch == "n":
                out.append("\n")
            elif ch == "r":
                out.append("\r")
            elif ch == "t":
                out.append("\t")
            elif ch == "\\":
                out.append("\\")
            else:
                out.append(ch)
        else:
            out.append(raw[i])
        i += 1
    return "".join(out)


def escape_cpp_string(text):
    out = []
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        else:
            out.append(ch)
    return "".join(out)


def fmt_float(value):
    return f"{float(value):g}"


def fmt_bool(value):
    return "true" if value else "false"


def convert(text):
    lines = text.splitlines()
    if not lines:
        raise ValueError("Empty input recording")
    header = lines[0].split()
    if len(header) != 4 or header[0] != "duckmidi-input" or header[1] != "1":
        raise ValueError(f"Unsupported recording header: {lines[0]!r}")

    frames = {}
    for line_no, line in enumerate(lines[1:], start=2):
        if not line.strip():
            continue
        parts = line.split(" ", 2)
        if len(parts) < 2:
            raise ValueError(f"Line {line_no}: malformed event {line!r}")
        frame_num = int(parts[0])
        kind = parts[1]
        rest = parts[2] if len(parts) > 2 else ""
        frames.setdefault(frame_num, []).append((kind, rest, line_no))

    x = -100.0
    y = -100.0
    left = False
    right = False
    alt = False
    shift = False
    ctrl = False
    emitted_ctrl = False
    emitted_shift = False
    emitted_alt = False

    out = [f"    // Window size: {header[2]}x{header[3]}"]
    for frame_num in sorted(frames):
        for kind, rest, line_no in frames[frame_num]:
            if kind == "motion":
                sx, sy = rest.split()
                x, y = float(sx), float(sy)
            elif kind in ("button_down", "button_up"):
                sb, sx, sy = rest.split()
                button = int(sb)
                x, y = float(sx), float(sy)
                down = kind == "button_down"
                if button == SDL_BUTTON_LEFT:
                    left = down
                elif button == SDL_BUTTON_RIGHT:
                    right = down
                if button in SDL_TO_IMGUI_BUTTON:
                    out.append(
                        f"    ImGui::GetIO().AddMousePosEvent({fmt_float(x)}, {fmt_float(y)});"
                    )
                    out.append(
                        f"    ImGui::GetIO().AddMouseButtonEvent("
                        f"{SDL_TO_IMGUI_BUTTON[button]}, {fmt_bool(down)});"
                    )
            elif kind == "wheel":
                wx, wy, mx, my = rest.split()
                x, y = float(mx), float(my)
                out.append(
                    f"    ImGui::GetIO().AddMousePosEvent({fmt_float(x)}, {fmt_float(y)});"
                )
                out.append(
                    f"    ImGui::GetIO().AddMouseWheelEvent({fmt_float(wx)}, {fmt_float(wy)});"
                )
            elif kind in ("key_down", "key_up"):
                ss, sm, sr = rest.split()
                scancode = int(ss)
                mod = int(sm)
                repeat = int(sr)
                down = kind == "key_down"
                if down and repeat:
                    continue
                if scancode in MODIFIER_SCANCODES:
                    mod_name = MODIFIER_SCANCODES[scancode]
                    if mod_name == "ctrl":
                        ctrl = down
                    elif mod_name == "shift":
                        shift = down
                    elif mod_name == "alt":
                        alt = down
                else:
                    ctrl = bool(mod & SDL_KMOD_CTRL)
                    shift = bool(mod & SDL_KMOD_SHIFT)
                    alt = bool(mod & SDL_KMOD_ALT)
                if ctrl != emitted_ctrl:
                    out.append(
                        f"    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, {fmt_bool(ctrl)});"
                    )
                    emitted_ctrl = ctrl
                if shift != emitted_shift:
                    out.append(
                        f"    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, {fmt_bool(shift)});"
                    )
                    emitted_shift = shift
                if alt != emitted_alt:
                    out.append(
                        f"    ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, {fmt_bool(alt)});"
                    )
                    emitted_alt = alt
                if scancode not in MODIFIER_SCANCODES:
                    if scancode in SCANCODES:
                        out.append(
                            f"    ImGui::GetIO().AddKeyEvent("
                            f"{SCANCODES[scancode]}, {fmt_bool(down)});"
                        )
                    elif down:
                        out.append(f"    /* unknown scancode {scancode} */")
            elif kind == "text":
                utf8 = unescape_text(rest)
                out.append(
                    f'    ImGui::GetIO().AddInputCharactersUTF8("{escape_cpp_string(utf8)}");'
                )
            else:
                raise ValueError(f"Line {line_no}: unknown event kind {kind!r}")

        out.append(
            f"    frame(e, {fmt_float(x)}, {fmt_float(y)}, {fmt_bool(left)}, "
            f"{fmt_bool(right)}, {fmt_bool(alt)}, {fmt_bool(shift)});"
        )

    return "\n".join(out) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Convert a duckmidi DEV input recording into a C++ test body."
    )
    parser.add_argument("recording", type=Path, help="Path to the duckmidi-input recording file.")
    args = parser.parse_args(argv)
    sys.stdout.write(convert(args.recording.read_text(encoding="utf-8")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
