# Minimal Windows audio libraries

The Windows SDK in `vendor/ffmpeg-9.0/` contains FFmpeg 9.0.2 shared libraries built for duckmidi's audio exports. LAME 3.100, Ogg 1.3.6, Vorbis 1.3.7, and Opus 1.6 are linked into those libraries.

## Rebuild the SDK

Install [MSYS2](https://www.msys2.org/). In the **MSYS2** terminal, install the build tools if needed:

```sh
pacman -S --needed make pkgconf diffutils perl tar curl mingw-w64-ucrt-x86_64-gcc
```

From the project root, run:

```console
& C:/msys64/usr/bin/bash.exe tools/build-ffmpeg-minimal.sh
```

The script downloads pinned source archives, checks their SHA-256 hashes, and builds under `build/development/ffmpeg-minimal-work/`. It does not install system packages. The result is `build/development/ffmpeg-minimal-work/sdk/`, containing `bin/`, `include/`, and `lib/`.

Replace the matching SDK directories in `vendor/ffmpeg-9.0/` with those staged directories, then run the normal CMake Release build; see the [project build instructions](../README.md#building).