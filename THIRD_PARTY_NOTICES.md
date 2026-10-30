# Third-party components

*duckmidi* uses the [MIT license](LICENSE).

Dependency licenses are kept in `vendor/licenses/` and the vendored source trees. Production packages include FluidR3; packaged notices and license texts are in `licenses/`.

| Component | Use | License text | Upstream source |
| --- | --- | --- | --- |
| Iconoir | Transport, editing and status icons plus cached native cursors, baked from SVG | [MIT](vendor/licenses/Iconoir-MIT.txt); source SVGs and license in `assets/iconoir/`, with local `assets/cut-copy.svg` | https://github.com/iconoir-icons/iconoir |
| Dear ImGui 1.92.5 | GUI, including bundled font/helper code | [MIT](vendor/licenses/Dear-ImGui-MIT.txt); source comments retain embedded helper notices | https://github.com/ocornut/imgui/tree/v1.92.5 |
| SDL 3.2.28 | GPU renderer, audio, dialogs and platform services | [zlib](vendor/licenses/SDL3-zlib.txt) | https://github.com/libsdl-org/SDL/tree/release-3.2.28 |
| RtMidi 6.0.0 | External MIDI and C wrapper | [MIT](vendor/licenses/RtMidi-MIT.txt) | https://github.com/thestk/rtmidi/tree/6.0.0 |
| FluidSynth 2.5.4 | Dynamically linked synthesizer | [LGPL 2.1](vendor/licenses/FluidSynth-LGPL.txt), upstream permits 2.1 or later | https://github.com/FluidSynth/fluidsynth/tree/v2.5.4 |
| FFmpeg 9.0.2 | Dynamically linked audio encoders and containers; minimal Windows LGPL build | [LGPL 3](vendor/licenses/FFmpeg-LGPL3.txt), incorporating [GPL 3](vendor/licenses/GCC-GPL3.txt) | https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz |
| FluidR3 GM | Default SoundFont in production packages | [MIT](vendor/licenses/FluidR3-LICENSE.txt) | https://sources.debian.org/src/fluid-soundfont/ |
| GLib / GObject / GThread | FluidSynth runtime dependencies | [LGPL](vendor/licenses/GLib-LGPL.txt) | https://gitlab.gnome.org/GNOME/glib |
| libsndfile | Sample/audio file dependency | [LGPL](vendor/licenses/libsndfile-LGPL.txt) | https://github.com/libsndfile/libsndfile |
| libintl | Runtime internationalization dependency | [LGPL](vendor/licenses/libintl-LGPL.txt) | https://www.gnu.org/software/gettext/ |
| GCC libgcc / libstdc++ | Compiler runtime, statically linked | [GPL3](vendor/licenses/GCC-GPL3.txt) with [GCC Runtime Library Exception](vendor/licenses/GCC-runtime-exception.txt) | https://gcc.gnu.org/ |
| MinGW-w64 runtime | Windows compiler/runtime support | [Runtime notices](vendor/licenses/MinGW-runtime.txt) | https://www.mingw-w64.org/ |
| winpthreads | Runtime thread support DLL | [COPYING](vendor/licenses/winpthreads-COPYING.txt) | https://github.com/mingw-w64/mingw-w64/tree/master/mingw-w64-libraries/winpthreads |
| FLAC | Lossless audio codec | [BSD notice](vendor/licenses/FLAC-COPYING.txt) | https://github.com/xiph/flac |
| Ogg 1.3.6 | Audio container support | [BSD notice](vendor/licenses/Ogg-COPYING.txt) | https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.xz |
| Vorbis 1.3.7 | Ogg Vorbis encoding and sample decoding | [BSD notice](vendor/licenses/Vorbis-COPYING.txt) | https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz |
| Opus 1.6 | Opus encoding and sample decoding | [COPYING](vendor/licenses/Opus-COPYING.txt) | https://downloads.xiph.org/releases/opus/opus-1.6.tar.gz |
| mpg123 | Decoder embedded in the upstream libsndfile binary | [LGPL 2.1 and notices](vendor/licenses/mpg123-COPYING.txt) | https://www.mpg123.de/ |
| LAME 3.100 | MP3 encoding through FFmpeg | [Library GPL 2](vendor/licenses/LAME-COPYING.txt) | https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz |

## M64 format references

The native exporter follows the sequence format and bank JSON schema in [n64decomp/sm64](https://github.com/n64decomp/sm64) (CC0-1.0).
