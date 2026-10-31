#!/usr/bin/env bash
# Internal recipe for Windows (MSYS2) and macOS; launch with tools/build-ffmpeg-minimal.cmake.
# Linux also works for local checks; Linux CI uses the distribution's FFmpeg.
# Keep it compatible with macOS's bash 3.2: no empty "${array[@]}" under set -u.
set -euo pipefail
case "$(uname -s)" in
    MINGW* | MSYS*) platform=windows ;;
    Darwin) platform=macos ;;
    Linux) platform=linux ;;
    *)
        echo "Unsupported build platform: $(uname -s)." >&2
        exit 1
        ;;
esac
root=$(cd "$(dirname "$0")/.." && pwd)
work="$root/build/development/ffmpeg-minimal-work"
prefix="$work/prefix"
reports="$root/agents/reports/ffmpeg-minimal"
export CFLAGS='-O2' CXXFLAGS='-O2'
if [[ $platform == windows ]]; then
    export PATH="/ucrt64/bin:/usr/bin:$PATH"
    export CC=gcc CXX=g++ AR=ar RANLIB=ranlib
    export PKG_CONFIG=/usr/bin/pkg-config
    dependency_flags="--host=x86_64-w64-mingw32"
    ffmpeg_flags="--target-os=mingw32 --arch=x86_64 --disable-schannel --extra-libs=-lssp"
    ffmpeg_ldflags="-static-libgcc"
    jobs=${JOBS:-4}
else
    export CC=${CC:-cc} CXX=${CXX:-c++}
    PKG_CONFIG=$(command -v pkg-config)
    export PKG_CONFIG
    # The static codec libraries are linked into shared FFmpeg libraries.
    dependency_flags="--with-pic"
    ffmpeg_flags=""
    ffmpeg_ldflags=""
    if [[ $platform == macos ]]; then
        # Matches LSMinimumSystemVersion in tools/package-macos.sh.
        export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-13.0}
        # The bundled config.guess scripts predate Apple Silicon.
        triple="$(uname -m | sed 's/^arm64$/aarch64/')-apple-darwin"
        dependency_flags="$dependency_flags --build=$triple --host=$triple"
        jobs=${JOBS:-$(sysctl -n hw.ncpu)}
    else
        jobs=${JOBS:-$(nproc)}
    fi
fi
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
mkdir -p "$work/downloads" "$work/sources" "$work/objects" "$work/tmp" "$prefix" "$reports"
export TMPDIR="$work/tmp"
exec > >(tee "$reports/build.log") 2>&1

# macOS's sha256sum lacks GNU options, so compare the printed digest instead of using --check.
check_hash() {
    local expected=$1 file=$2 actual
    if [[ $platform == macos ]]; then
        actual=$(shasum -a 256 "$file" | awk '{print $1}')
    else
        actual=$(sha256sum "$file" | awk '{print $1}')
    fi
    if [[ $actual != "$expected" ]]; then
        echo "SHA-256 mismatch for $file: expected $expected, got $actual." >&2
        exit 1
    fi
    echo "$file: OK"
}

fetch() {
    local name=$1 url=$2 hash=$3
    if [[ ! -f "$work/downloads/$name" ]]; then
        curl --fail --location --retry 2 --output "$work/downloads/$name" "$url"
    fi
    check_hash "$hash" "$work/downloads/$name"
}
fetch ffmpeg-9.0.2.tar.xz https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
fetch lame-3.100.tar.gz https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e
fetch libogg-1.3.6.tar.xz https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.xz 5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061
fetch libvorbis-1.3.7.tar.xz https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b
fetch opus-1.6.tar.gz https://downloads.xiph.org/releases/opus/opus-1.6.tar.gz b7637334527201fdfd6dd6a02e67aceffb0e5e60155bbd89175647a80301c92c
for archive in "$work"/downloads/*; do tar -xf "$archive" -C "$work/sources"; done
if [[ $platform == macos ]]; then
    # Current Apple linkers reject this flag from libvorbis's Darwin defaults.
    perl -pi -e 's/ -force_cpusubtype_ALL//g' "$work/sources/libvorbis-1.3.7/configure"
fi

build_dependency() {
    local name=$1
    shift
    mkdir -p "$work/objects/$name"
    (
        cd "$work/objects/$name"
        # shellcheck disable=SC2086 # dependency_flags holds several words.
        "$work/sources/$name/configure" $dependency_flags --prefix="$prefix" \
            --disable-shared --enable-static --disable-dependency-tracking ${1+"$@"}
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
# shellcheck disable=SC2086 # ffmpeg_flags holds several words.
"$work/sources/ffmpeg-9.0.2/configure" \
    --prefix="$prefix" --cc="$CC" --cxx="$CXX" $ffmpeg_flags \
    --disable-autodetect --disable-everything --disable-programs --disable-doc \
    --disable-debug --disable-network --disable-avdevice --disable-avfilter --disable-swscale \
    --disable-x86asm --disable-iconv --disable-sdl2 \
    --enable-shared --disable-static --enable-version3 --disable-gpl --disable-nonfree \
    --enable-small --enable-avcodec --enable-avformat --enable-avutil --enable-swresample \
    --enable-libmp3lame --enable-libvorbis --enable-libopus \
    --enable-encoder=pcm_s16le,pcm_s24le,libmp3lame,aac,libvorbis,libopus,flac \
    --enable-muxer=wav,mp3,ipod,ogg,opus,flac \
    --pkg-config="$PKG_CONFIG" --pkg-config-flags=--static \
    --extra-cflags="-I$prefix/include" \
    --extra-ldflags="-L$prefix/lib $ffmpeg_ldflags"
make -j"$jobs"
make install
cp config.h config_components.h ffbuild/config.mak "$reports/"
if [[ $platform != windows ]]; then
    echo "FFmpeg installed at $prefix. Configure duckmidi with -DCMAKE_PREFIX_PATH=$prefix."
    exit 0
fi
sdk="$work/sdk"
mkdir -p "$sdk/bin" "$sdk/lib" "$sdk/include"
for component in avcodec avformat avutil swresample; do
    cp "$prefix/bin/$component"-*.dll "$sdk/bin/"
    cp "$prefix/lib/lib$component.dll.a" "$sdk/lib/"
    cp -R "$prefix/include/lib$component" "$sdk/include/"
done
cp /ucrt64/bin/libwinpthread-1.dll "$sdk/bin/"
echo "Minimal SDK staged at $sdk. Validate before copying bin/include/lib into vendor/ffmpeg-9.0."
