# FFmpeg

Minimal Windows audio SDK built from FFmpeg 9.0.2 with GCC for x86-64 UCRT. It includes the avcodec, avformat, avutil, and swresample shared libraries, development headers, GNU import libraries, and the required winpthreads runtime DLL.

Audio export supports 16/24-bit PCM, LAME MP3, native AAC, Vorbis, Opus, and FLAC. The linked codec dependencies are LAME 3.100, Ogg 1.3.6, Vorbis 1.3.7, and Opus 1.6. Decoders, input formats, video encoders, network protocols, devices, filters, and command-line programs are disabled. FFmpeg enables internal dependencies required by the selected audio muxers.

The build enables LGPL version 3 and disables GPL and nonfree components. Codec dependencies are statically linked into the FFmpeg DLLs; duckmidi dynamically links those DLLs.

The [build recipe](../../tools/build-ffmpeg-minimal.sh) pins source URLs and SHA-256 hashes. See [rebuild and validation instructions](../../docs/ffmpeg-build.md). Temporary sources, downloads, and objects can be removed after validation; this SDK remains sufficient for normal application builds.

Source archives:

- https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
- https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz
- https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.xz
- https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz
- https://downloads.xiph.org/releases/opus/opus-1.6.tar.gz

See `../licenses/FFmpeg-LGPL3.txt` and `../../THIRD_PARTY_NOTICES.md`.
