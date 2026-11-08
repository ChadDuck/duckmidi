# Rebuilding FFmpeg

duckmidi uses a trimmed-down FFmpeg 9.0.2 for reading and exporting audio, with LAME 3.100, Ogg 1.3.6, Vorbis 1.3.7 and Opus 1.6 for the export codecs. It can read WAV, MP3, AAC (in M4A), Ogg Vorbis, Ogg Opus, FLAC and AIFF, and leaves out all GPL and nonfree parts.

You only need this page to rebuild those libraries. On Windows, a ready-made copy is already in `vendor/ffmpeg-9.0/`. On macOS you have to build it yourself, because Homebrew's `ffmpeg` doesn't include the Vorbis encoder. Linux uses your distribution's FFmpeg instead.

## Windows

1. Install [MSYS2](https://www.msys2.org/) and [CMake](https://cmake.org/download/) 3.22+, and add CMake to your PATH.
2. In the **MSYS2** terminal, install the build tools:

   ```sh
   pacman -S --needed make pkgconf diffutils perl tar curl mingw-w64-ucrt-x86_64-gcc
   ```

3. From the project folder, run:

   ```console
   cmake -P tools/build-ffmpeg-minimal.cmake
   ```

   If MSYS2 isn't in `C:/msys64`, point to it: `cmake "-DMSYS2_ROOT=D:/msys64" -P tools/build-ffmpeg-minimal.cmake`.

4. The new libraries land in `build/development/ffmpeg-minimal-work/sdk/`. To use them, copy them over the matching folders in `vendor/ffmpeg-9.0/`.

## macOS

1. Install the Command Line Tools (`xcode-select --install`) and [Homebrew](https://brew.sh/).
2. In **Terminal**, install the build tools and run the recipe from the project folder:

   ```sh
   brew install cmake pkgconf
   cmake -P tools/build-ffmpeg-minimal.cmake
   ```

3. The libraries land in `build/development/ffmpeg-minimal-work/prefix/`. List that folder first when you configure duckmidi:

   ```sh
   cmake -S . -B build/development/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/build/development/ffmpeg-minimal-work/prefix;$(brew --prefix)"
   ```

Don't install Homebrew's `ffmpeg` for duckmidi builds. duckmidi links to the libraries in `prefix/`, and release packages copy them into `duckmidi.app`.

## Good to know

To check your tools without downloading or building anything:

```console
cmake -DCHECK_ONLY=ON -P tools/build-ffmpeg-minimal.cmake
```

The recipe downloads fixed source versions, checks their SHA-256 hashes and builds everything in `build/development/ffmpeg-minimal-work/`. Build logs go to `agents/reports/ffmpeg-minimal/`.

To build duckmidi itself, see [Building](../README.md#building).
