#!/usr/bin/env bash
# Packages a configured Linux build as an Arch package or a Debian/Ubuntu .deb.
# Usage: tools/package-linux.sh arch|deb BUILD_DIR VERSION OUTPUT_DIR
# Files install to /usr/lib/duckmidi with a /usr/bin/duckmidi link.
# For deb, set FLUIDSYNTH_LIB to a folder holding a FluidSynth 2.5.4+ build to bundle;
# configure that build with -DCMAKE_INSTALL_RPATH='$ORIGIN/lib'.
# Arch packages are built with makepkg, which must run as a regular user.
set -euo pipefail

format=$1
build=$2
version=$3
output=$(realpath -m "$4")
source=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
root="$work/root"
app="$root/usr/lib/duckmidi"

cmake --install "$build" --prefix "$app" --strip
mkdir -p "$root/usr/bin" "$root/usr/share/applications" "$root/usr/share/licenses/duckmidi"
ln -s ../lib/duckmidi/duckmidi "$root/usr/bin/duckmidi"
cp "$source/LICENSE" "$root/usr/share/licenses/duckmidi/LICENSE"
cat > "$root/usr/share/applications/duckmidi.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=duckmidi
Comment=MIDI editor
Exec=duckmidi
Terminal=false
Categories=AudioVideo;Audio;Midi;
EOF
mkdir -p "$output"

case "$format" in
arch)
  cat > "$work/PKGBUILD" <<EOF
pkgname=duckmidi
pkgver=${version}
pkgrel=1
pkgdesc="MIDI editor"
arch=(x86_64)
url="https://github.com/ChadDuck/duckmidi"
license=(MIT)
depends=(sdl3 fluidsynth alsa-lib ffmpeg gcc-libs glibc)
options=(!strip !debug)

package() {
  cp -a "${root}/." "\$pkgdir/"
}
EOF
  (cd "$work" && PKGDEST="$output" makepkg --force --nodeps)
  ;;
deb)
  if [ -z "${FLUIDSYNTH_LIB:-}" ]; then
    echo "Set FLUIDSYNTH_LIB to the folder holding libfluidsynth.so.3." >&2
    exit 1
  fi

  mkdir -p "$app/lib"
  cp -a "$FLUIDSYNTH_LIB"/libfluidsynth.so.3* "$app/lib/"
  strip --strip-unneeded "$app"/lib/libfluidsynth.so.3.*
  . /etc/os-release
  distro="${ID}${VERSION_ID}"
  mkdir -p "$work/debian" "$root/DEBIAN"
  echo "Source: duckmidi" > "$work/debian/control"
  depends=$(cd "$work" && dpkg-shlibdeps -O --ignore-missing-info -l"$app/lib" "$app/duckmidi" "$app"/lib/libfluidsynth.so.3.* "$app"/plugins/*.so)
  depends=${depends#shlibs:Depends=}
  cat > "$root/DEBIAN/control" <<EOF
Package: duckmidi
Version: ${version}
Architecture: amd64
Maintainer: ChadDuck <https://github.com/ChadDuck/duckmidi>
Depends: ${depends}
Section: sound
Priority: optional
Homepage: https://github.com/ChadDuck/duckmidi
Description: MIDI editor
 duckmidi edits, plays and exports MIDI projects.
EOF
  dpkg-deb --root-owner-group --build "$root" "$output/duckmidi_${version}_${distro}_amd64.deb"
  ;;
*)
  echo "Unknown format: $format (use arch or deb)" >&2
  exit 1
  ;;
esac
