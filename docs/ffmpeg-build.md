# Minimal audio libraries

duckmidi's Windows and macOS builds use FFmpeg 9.0.2 shared libraries built for its audio exports. LAME 3.100, Ogg 1.3.6, Vorbis 1.3.7, and Opus 1.6 are linked into those libraries, so every export format has its encoder.

The Windows SDK is checked in under `vendor/ffmpeg-9.0/`. macOS builds compile the same libraries locally, because Homebrew's `ffmpeg` package lacks the Vorbis encoder.

## Windows

Install [MSYS2](https://www.msys2.org/). In the **MSYS2** terminal, install the build tools if needed:

```sh
pacman -S --needed make pkgconf diffutils perl tar curl mingw-w64-ucrt-x86_64-gcc
```

Install [CMake](https://cmake.org/download/) 3.22 or newer and add it to PATH. From the project root, run:

```console
cmake -P tools/build-ffmpeg-minimal.cmake
```

The launcher uses `C:/msys64` by default. For another installation, run `cmake -DMSYS2_ROOT=D:/msys64 -P tools/build-ffmpeg-minimal.cmake` (quote the entire `-DMSYS2_ROOT=...` argument if its path contains spaces).

The result is `build/development/ffmpeg-minimal-work/sdk/`. Replace the matching SDK directories in `vendor/ffmpeg-9.0/` with those staged directories when ready to use the rebuilt SDK.

## macOS

Install the Xcode Command Line Tools (`xcode-select --install`) and [Homebrew](https://brew.sh/). In **Terminal**, install the build tools if needed:

```sh
brew install cmake pkgconf
```

From the project root, run:

```sh
cmake -P tools/build-ffmpeg-minimal.cmake
```

The libraries are installed to `build/development/ffmpeg-minimal-work/prefix/`. Put that folder first when configuring duckmidi:

```sh
cmake -S . -B build/development/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/build/development/ffmpeg-minimal-work/prefix;$(brew --prefix)"
```

Do not install Homebrew's `ffmpeg` for duckmidi builds. The build links these libraries by their location in `prefix/`; release packaging copies them into `duckmidi.app`.

## Common details

To check prerequisites and recipe syntax without downloading or compiling, run:

```console
cmake -DCHECK_ONLY=ON -P tools/build-ffmpeg-minimal.cmake
```

The recipe downloads pinned source archives, checks their SHA-256 hashes, and builds under `build/development/ffmpeg-minimal-work/`. Build logs and configuration captures go under `agents/reports/ffmpeg-minimal/`. This rebuild compiles only FFmpeg and its codec dependencies.

Linux builds use the distribution's FFmpeg packages, which include these encoders. Application compilation is a separate step; see [project build instructions](../README.md#building).
