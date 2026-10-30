#!/usr/bin/env bash
# Internal MSYS2 recipe; launch with tools/build-ffmpeg-minimal.cmake.
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"
root=$(cd "$(dirname "$0")/.." && pwd)
work="$root/build/development/ffmpeg-minimal-work"
prefix="$work/prefix"
reports="$root/agents/reports/ffmpeg-minimal"
export CC=gcc CXX=g++ AR=ar RANLIB=ranlib
export CFLAGS='-O2' CXXFLAGS='-O2'
export PKG_CONFIG=/usr/bin/pkg-config
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
jobs=${JOBS:-4}
mkdir -p "$work/downloads" "$work/sources" "$work/objects" "$work/tmp" "$prefix" "$reports"
export TMPDIR="$work/tmp"
exec > >(tee "$reports/build.log") 2>&1

fetch() {
    local name=$1 url=$2 hash=$3
    if [[ ! -f "$work/downloads/$name" ]]; then
        curl --fail --location --retry 2 --output "$work/downloads/$name" "$url"
    fi
    echo "$hash  $work/downloads/$name" | sha256sum --check
}
fetch ffmpeg-9.0.2.tar.xz https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
fetch lame-3.100.tar.gz https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
fetch libogg-1.3.6.tar.xz https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.xz 5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061
fetch libvorbis-1.3.7.tar.xz https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b
fetch opus-1.6.tar.gz https://downloads.xiph.org/releases/opus/opus-1.6.tar.gz b7637334527201fdfd6dd6a02e67aceffb0e5e60155bbd89175647a80301c92c
for archive in "$work"/downloads/*; do tar -xf "$archive" -C "$work/sources"; done

build_dependency() {
    local name=$1; shift
    mkdir -p "$work/objects/$name"
    (
        cd "$work/objects/$name"
        "$work/sources/$name/configure" --host=x86_64-w64-mingw32 --prefix="$prefix" \
            --disable-shared --enable-static --disable-dependency-tracking "$@"
        make -j"$jobs"
        make install
    )
}
build_dependency libogg-1.3.6
build_dependency libvorbis-1.3.7 --disable-docs --disable-examples
build_dependency opus-1.6 --disable-doc --disable-extra-programs --disable-dred --disable-osce
build_dependency lame-3.100 --disable-frontend --disable-decoder --disable-analyzer-hooks

mkdir -p "$work/objects/ffmpeg"
cd "$work/objects/ffmpeg"
"$work/sources/ffmpeg-9.0.2/configure" \
    --prefix="$prefix" --target-os=mingw32 --arch=x86_64 --cc=gcc --cxx=g++ \
    --disable-autodetect --disable-everything --disable-programs --disable-doc \
    --disable-debug --disable-network --disable-avdevice --disable-avfilter --disable-swscale \
    --disable-x86asm --disable-iconv --disable-schannel --disable-sdl2 \
    --enable-shared --disable-static --enable-version3 --disable-gpl --disable-nonfree \
    --enable-small --enable-avcodec --enable-avformat --enable-avutil --enable-swresample \
    --enable-libmp3lame --enable-libvorbis --enable-libopus \
    --enable-encoder=pcm_s16le,pcm_s24le,libmp3lame,aac,libvorbis,libopus,flac \
    --enable-muxer=wav,mp3,ipod,ogg,opus,flac \
    --pkg-config="$PKG_CONFIG" --pkg-config-flags=--static \
    --extra-cflags="-I$prefix/include" \
    --extra-ldflags="-L$prefix/lib -static-libgcc" \
    --extra-libs=-lssp
make -j"$jobs"
make install
cp config.h config_components.h ffbuild/config.mak "$reports/"
sdk="$work/sdk"
mkdir -p "$sdk/bin" "$sdk/lib" "$sdk/include"
for component in avcodec avformat avutil swresample; do
    cp "$prefix/bin/$component"-*.dll "$sdk/bin/"
    cp "$prefix/lib/lib$component.dll.a" "$sdk/lib/"
    cp -R "$prefix/include/lib$component" "$sdk/include/"
done
cp /ucrt64/bin/libwinpthread-1.dll "$sdk/bin/"
echo "Minimal SDK staged at $sdk. Validate before copying bin/include/lib into vendor/ffmpeg-9.0."
