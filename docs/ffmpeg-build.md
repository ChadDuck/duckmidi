# Minimal Windows audio libraries

The Windows SDK in `vendor/ffmpeg-9.0/` contains FFmpeg 9.0.2 shared libraries built for duckmidi's audio exports. LAME 3.100, Ogg 1.3.6, Vorbis 1.3.7, and Opus 1.6 are linked into those libraries.

## Rebuild the SDK

Install [MSYS2](https://www.msys2.org/). In the **MSYS2** terminal, install the build tools if needed:

```sh
pacman -S --needed make pkgconf diffutils perl tar curl mingw-w64-ucrt-x86_64-gcc
```

Install [CMake](https://cmake.org/download/) 3.22 or newer and add it to PATH. From the project root, run:

```console
cmake -P tools/build-ffmpeg-minimal.cmake
```

The launcher uses `C:/msys64` by default. For another installation, run `cmake -DMSYS2_ROOT=D:/msys64 -P tools/build-ffmpeg-minimal.cmake` (quote the entire `-DMSYS2_ROOT=...` argument if its path contains spaces).

To check prerequisites and recipe syntax without downloading or compiling, run:

```console
cmake -DCHECK_ONLY=ON -P tools/build-ffmpeg-minimal.cmake
```

The recipe downloads pinned source archives, checks their SHA-256 hashes, and builds under `build/development/ffmpeg-minimal-work/`. The result is `build/development/ffmpeg-minimal-work/sdk/`. Build logs and configuration captures go under `agents/reports/ffmpeg-minimal/`. This rebuild compiles only FFmpeg and its codec dependencies.

Replace the matching SDK directories in `vendor/ffmpeg-9.0/` with those staged directories when ready to use the rebuilt SDK. Application compilation is a separate step; see [project build instructions](../README.md#building).