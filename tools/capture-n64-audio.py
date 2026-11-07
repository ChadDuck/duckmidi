#!/usr/bin/env python3
"""Capture audio from a staged N64 ROM with a ParaLLEl N64 libretro core."""

import argparse
import ctypes as c
import wave
from pathlib import Path


class Game(c.Structure):
    _fields_ = [("path", c.c_char_p), ("data", c.c_void_p),
                ("size", c.c_size_t), ("meta", c.c_char_p)]


class Variable(c.Structure):
    _fields_ = [("key", c.c_char_p), ("value", c.c_char_p)]


class Geometry(c.Structure):
    _fields_ = [("base_width", c.c_uint), ("base_height", c.c_uint),
                ("max_width", c.c_uint), ("max_height", c.c_uint), ("aspect_ratio", c.c_float)]


class Timing(c.Structure):
    _fields_ = [("fps", c.c_double), ("rate", c.c_double)]


class AudioVideo(c.Structure):
    _fields_ = [("geometry", Geometry), ("timing", Timing)]


def capture(core_path, rom_path, output, frames):
    core = c.CDLL(str(core_path))
    directories = bytes(output.parent)
    audio = bytearray()
    options = {
        b"parallel-n64-gfxplugin": b"angrylion",
        b"parallel-n64-rspplugin": b"cxd4",
        b"parallel-n64-angrylion-multithread": b"disabled",
    }

    @c.CFUNCTYPE(c.c_bool, c.c_uint, c.c_void_p)
    def environment(command, data):
        if command == 15:  # RETRO_ENVIRONMENT_GET_VARIABLE
            variable = c.cast(data, c.POINTER(Variable)).contents
            variable.value = options.get(variable.key)
            return variable.value is not None
        if command in (9, 30, 31):  # System, assets and save directories.
            c.cast(data, c.POINTER(c.c_char_p))[0] = directories
            return True
        if command in (3, 17):  # GET_CAN_DUPE and GET_VARIABLE_UPDATE.
            c.cast(data, c.POINTER(c.c_bool))[0] = command == 3
            return True
        if command == 52:  # GET_CORE_OPTIONS_VERSION, use legacy options.
            c.cast(data, c.POINTER(c.c_uint))[0] = 0
            return True
        return command in (10, 11, 16, 18, 32, 37)

    @c.CFUNCTYPE(None, c.c_void_p, c.c_uint, c.c_uint, c.c_size_t)
    def video(_data, _width, _height, _pitch):
        pass

    @c.CFUNCTYPE(None, c.c_int16, c.c_int16)
    def audio_sample(left, right):
        audio.extend(bytes((c.c_int16 * 2)(left, right)))

    @c.CFUNCTYPE(c.c_size_t, c.POINTER(c.c_int16), c.c_size_t)
    def audio_batch(data, count):
        audio.extend(c.string_at(data, count * 4))
        return count

    @c.CFUNCTYPE(None)
    def poll():
        pass

    @c.CFUNCTYPE(c.c_int16, c.c_uint, c.c_uint, c.c_uint, c.c_uint)
    def input_state(_port, _device, _index, _button):
        return 0

    for name, callback in (("environment", environment), ("video_refresh", video),
                           ("audio_sample", audio_sample), ("audio_sample_batch", audio_batch),
                           ("input_poll", poll), ("input_state", input_state)):
        register = getattr(core, f"retro_set_{name}")
        register.argtypes = [type(callback)]
        register.restype = None
        register(callback)

    core.retro_load_game.argtypes = [c.POINTER(Game)]
    core.retro_load_game.restype = c.c_bool
    core.retro_get_system_av_info.argtypes = [c.POINTER(AudioVideo)]
    rom = rom_path.read_bytes()
    buffer = c.create_string_buffer(rom)
    game = Game(bytes(rom_path), c.cast(buffer, c.c_void_p), len(rom), None)
    core.retro_init()
    if not core.retro_load_game(c.byref(game)):
        raise RuntimeError("The emulator rejected the staged ROM")
    av = AudioVideo()
    core.retro_get_system_av_info(c.byref(av))
    for _ in range(frames):
        core.retro_run()
    core.retro_unload_game()
    core.retro_deinit()
    if not audio or not 8000 <= av.timing.rate <= 192000:
        raise RuntimeError(f"No usable audio from the emulator (rate {av.timing.rate})")
    with wave.open(str(output), "wb") as wav:
        wav.setparams((2, 2, round(av.timing.rate), 0, "NONE", "not compressed"))
        wav.writeframes(audio)
    print(f"Captured {len(audio) / (4 * av.timing.rate):.2f} seconds at {av.timing.rate:g} Hz")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core", type=Path)
    parser.add_argument("rom", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--frames", type=int, default=720)
    args = parser.parse_args()
    if args.frames < 1:
        parser.error("--frames must be positive")
    capture(args.core.resolve(strict=True), args.rom.resolve(strict=True),
            args.output.resolve(), args.frames)


if __name__ == "__main__":
    main()
