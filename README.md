![duckmidi banner](assets/duckmidi-header.jpg)

# *duckmidi*

A MIDI editor for the modern age, made with love. 💛

*duckmidi* aims to offer a simple workspace for compositions, arrangements, or just listening to your favorite MIDI tunes. It combines familiar music-editing tools with quality-of-life features that make working with MIDI 1.0 easier.

**This program is in active development; expect bugs.**

<img src="assets/duckmidi-preview.jpg" width="100%">

## \*Disclaimer\*

While the design, usability, UI, and goals of this program were dictated by a human, the codebase itself was **generated and modified using *Codex* & *Claude***.

## Capabilities

- Create, edit, import, and export MIDI files
   - You can import .MUS files too, if you're into that sort of thing :P.
- Arrange reusable note and automation patterns.
- Edit and automate MIDI events within the GUI - pitch bends, velocity, modulation, reverb, chorus, portamento, sustain, sostenuto, and more. Use LFOs for repeating effects that snap to the beat.
- Record piano notes with your keyboard! 
- Various quality of life options - wait-for-input recording, overlapping takes, and even a metronome!
- Customize keyboard shortcuts (and modifiers) and simple percussion key mappings.
- Give notes independent pitch bends and controller effects by assigning per-note MIDI ports. Up to 128 ports are supported. (Playback support depends on the receiving application).
- Use an SF2/SF3 SoundFont or an external MIDI output.
- Save compositions as `.duck` projects or retain editor data in exported MIDI files.
- Export MIDI, WAV (PCM), MP3, AAC, Ogg Vorbis, Opus, and FLAC.
- Add export formats through the plugin API. 
   - The bundled M64 plugin converts General MIDI arrangements into SM64 sequences; transforming a previously arduous task into a quick and painless experience.

## Performance

Estimates; tested against *Release* v0.10.2 using a complex midi composition.
- RAM Usage: ~150 MiB
- CPU Usage (All threads): ~3%
- GPU Usage: ~8%

## Planned

- Core
   - Performance
      - Improve Resource consumption
      - Improve GUI performance; it suffers with complex MIDI compositions
   - GUI
      - Pattern colors
      - 'Unslop' parts of the GUI
      - More keyboard shortcuts.
   - MIDI Input Support
   - Better implementation for Polyphonic Aftertouch
   - API Expansion
- Plugins
   - M64 - Export to repo
   - M64 - Automatic adjustment of sequence memory code to match export requirements
   - M64 - Loop Points
   - OoT/MM Exporting
- Probably a lot more


## Getting Started

*Full* builds include *TimGM6mb.sf2* as the default SoundFont and a sample project that opens on initial launch. 

*Core* builds contain only the *duckmidi* program and necessary libraries; you can select a local SoundFont in **Edit > Preferences**, then create a project or open a MIDI file.

See the [user guide](docs/user-guide.md), [M64 export guide](docs/sm64-export.md), and [plugin API reference](docs/plugin-api.md).

## Building

*duckmidi* builds with [CMake](https://cmake.org/download/) 3.22 or newer, Ninja, and a C17/C++17 compiler. Run every build command from the project folder, the one containing `CMakeLists.txt`.

Windows builds use the SDL3, FluidSynth, and FFmpeg libraries included in `vendor/`. Other platforms use installed packages: SDL 3.2 or newer, FluidSynth 2.5.4 or newer, and FFmpeg 7.1 or newer with the LAME, Vorbis, and Opus encoders.

The macOS and Linux steps have not been tested yet.

### Windows

1. Install [MSYS2](https://www.msys2.org/) to its default folder, `C:\msys64`.
2. Open the **MSYS2 UCRT64** terminal from the Start menu and update its packages:

   ```sh
   pacman -Syu
   ```

   If the update closes the terminal, reopen it and run the command again.

3. Install the compiler and build tools:

   ```sh
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
   ```

4. Go to the project folder, then configure, build, and test:

   ```sh
   cd /c/path/to/duckmidi
   cmake -S . -B build/development/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build/development/cmake
   ctest --test-dir build/development/cmake --output-on-failure
   ```

5. Launch the editor:

   ```sh
   ./build/development/cmake/bin/duckmidi.exe
   ```

The executable is `build/development/cmake/bin/duckmidi.exe`, with its DLLs and `plugins/` folder beside it. Choose `vendor/TimGM6mb.sf2` or another SoundFont in **Edit > Preferences**.

To build from Command Prompt or PowerShell instead, add `C:\msys64\ucrt64\bin` to your user PATH in Windows Environment Variables, reopen the terminal, and add `-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++` to the configure command. Launch `build\development\cmake\bin\duckmidi.exe`.

The included FFmpeg libraries contain only the audio-export components. To rebuild them, see [minimal FFmpeg build instructions](docs/ffmpeg-build.md).

### macOS

1. Open **Terminal** and install Apple's [Command Line Tools](https://developer.apple.com/documentation/xcode/installing-the-command-line-tools/):

   ```sh
   xcode-select --install
   ```

2. Install [Homebrew](https://brew.sh/) and follow its PATH instructions, then install the dependencies:

   ```sh
   brew install cmake ninja pkgconf sdl3 fluid-synth ffmpeg
   ```

3. From the project folder, build, test, and launch:

   ```sh
   cmake -S . -B build/development/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build/development/cmake
   ctest --test-dir build/development/cmake --output-on-failure
   ./build/development/cmake/bin/duckmidi
   ```

The executable is `build/development/cmake/bin/duckmidi`. If CMake cannot find the Homebrew packages, add `-DCMAKE_PREFIX_PATH="$(brew --prefix)"` to the first command.

### Arch Linux

1. In a terminal, install the compiler and [development packages](https://archlinux.org/packages/extra/x86_64/sdl3/):

   ```sh
   sudo pacman -Syu --needed base-devel cmake ninja pkgconf sdl3 fluidsynth alsa-lib ffmpeg
   ```

2. From the project folder, build, test, and launch:

   ```sh
   cmake -S . -B build/development/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build/development/cmake
   ctest --test-dir build/development/cmake --output-on-failure
   ./build/development/cmake/bin/duckmidi
   ```

The executable is `build/development/cmake/bin/duckmidi`.

### Ubuntu / Debian

These steps need Ubuntu 26.04+ or Debian 13+, which package SDL3. On older releases, [install SDL3 from source](https://wiki.libsdl.org/SDL3/README-linux) first.

1. In a terminal, install the build tools and development packages:

   ```sh
   sudo apt update
   sudo apt install build-essential cmake ninja-build pkg-config git libsdl3-dev libasound2-dev libglib2.0-dev libsndfile1-dev libavcodec-dev libavformat-dev libavutil-dev libswresample-dev
   ```

2. Distribution packages may ship an older FluidSynth. From the project folder, build [FluidSynth 2.5.4](https://github.com/FluidSynth/fluidsynth/tree/v2.5.4) into a local folder:

   ```sh
   git clone --depth 1 --branch v2.5.4 https://github.com/FluidSynth/fluidsynth.git build/development/deps/fluidsynth
   cmake -S build/development/deps/fluidsynth -B build/development/deps/fluidsynth-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX:PATH=build/development/deps/install -DCMAKE_INSTALL_LIBDIR=lib
   cmake --build build/development/deps/fluidsynth-build
   cmake --install build/development/deps/fluidsynth-build
   ```

3. Build, test, and launch *duckmidi* with that FluidSynth:

   ```sh
   cmake -S . -B build/development/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH:PATH=build/development/deps/install
   cmake --build build/development/cmake
   ctest --test-dir build/development/cmake --output-on-failure
   ./build/development/cmake/bin/duckmidi
   ```

The executable is `build/development/cmake/bin/duckmidi`. Select a SoundFont in **Edit > Preferences**.

### Package a Build

After building, create a delivery folder from the same terminal:

```sh
cmake --build build/development/cmake --target deliver
```

Each delivery goes to a new `build/YYYY-MM-DD_HH-mm-ss-vVERSION/` folder containing the executable, the M64 plugin, required runtime libraries, documentation, the changelog, and licenses.

| Option | Effect |
| --- | --- |
| `-DDUCKMIDI_PRODUCTION=ON` | Includes the *TimGM6mb* SoundFont and sample project (*Full* build). `OFF` packages only the application (*Core* build). |
| `-DDUCKMIDI_DELIVERY_TYPE=Full` or `Core` | Writes to `build/duckmidi-vVERSION-Full/` or `-Core/` instead of a dated folder, replacing the previous one. |

Add options to the configure command, then rebuild before delivering:

```sh
cmake -S . -B build/development/cmake -DDUCKMIDI_PRODUCTION=ON -DDUCKMIDI_DELIVERY_TYPE=Full
cmake --build build/development/cmake --target deliver
```

To stage the files in a folder of your choice instead, run `cmake --install build/development/cmake --prefix build/development/package --strip`.

## Libraries and Assets

*duckmidi* uses SDL3, Dear ImGui, FluidSynth, FFmpeg, RtMidi, and Iconoir artwork. *Full* builds include *TimGM6mb.sf2*. See [third-party notices](THIRD_PARTY_NOTICES.md) for licenses and sources.

The M64 converter's public API is in `include/m64export/`; its implementation and plugin adapter are in `libraries/m64export/`.

## Support and Development

When reporting a bug, include the application version, the steps that cause it, and what you expected to happen. Changes in each version are listed in the [changelog](CHANGELOG.md).

## License

*duckmidi* is available under the [MIT license](LICENSE). Dependencies, SoundFonts, and other third-party assets keep their own licenses.
