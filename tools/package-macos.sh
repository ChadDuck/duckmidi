#!/usr/bin/env bash
# Packages a configured macOS build as duckmidi.app inside a zip.
# Usage: tools/package-macos.sh BUILD_DIR VERSION OUTPUT_DIR
# Needs dylibbundler (brew install dylibbundler). The app is ad-hoc signed, not notarized.
set -euo pipefail

build=$1
version=$2
output=$3
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cmake --install "$build" --prefix "$work/root" --strip
app="$work/duckmidi.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$app/Contents/Frameworks"
mv "$work/root/duckmidi" "$app/Contents/MacOS/duckmidi"
# SDL_GetBasePath() returns Contents/Resources/ inside a bundle, so plugins and assets go there.
cp -R "$work/root/." "$app/Contents/Resources/"

cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>duckmidi</string>
  <key>CFBundleDisplayName</key><string>duckmidi</string>
  <key>CFBundleIdentifier</key><string>io.github.chadduck.duckmidi</string>
  <key>CFBundleExecutable</key><string>duckmidi</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>${version}</string>
  <key>CFBundleShortVersionString</key><string>${version}</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF

# Copy Homebrew libraries into the bundle and point the executable at them.
dylibbundler -of -b -cd -x "$app/Contents/MacOS/duckmidi" \
  -d "$app/Contents/Frameworks/" -p @executable_path/../Frameworks/
# Stripping removes the linker signature, so re-sign the plugins before the bundle.
codesign --force --sign - "$app"/Contents/Resources/plugins/*.dylib
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"

mkdir -p "$output"
ditto -c -k --keepParent "$app" "$output/duckmidi-v${version}-macos-$(uname -m).zip"
