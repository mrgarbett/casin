#!/bin/sh
# Builds OpenClone.app (Swift + AppKit) with SwiftPM. Run on macOS 13+ with
# Xcode or the Command Line Tools installed.
#   scripts/build_macos_app.sh            -> build/macos/OpenClone.app
set -eu
cd "$(dirname "$0")/.."

swift build -c release --product OpenClone
BIN="$(swift build -c release --show-bin-path)/OpenClone"

APP=build/macos/OpenClone.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/OpenClone"
# SwiftPM can record the deployment target (13.0) as the SDK version. AppKit
# keys the macOS 26 design (floating glass sidebar, glass toolbar) off the SDK
# version, so stamp the real one in; the minimum system version stays 13.0.
SDK_VERSION="$(xcrun --show-sdk-version 2>/dev/null || true)"
if [ -n "$SDK_VERSION" ] && command -v vtool >/dev/null 2>&1; then
    vtool -set-build-version macos 13.0 "$SDK_VERSION" -replace \
        -output "$APP/Contents/MacOS/OpenClone" "$BIN" 2>/dev/null ||
        echo "warning: vtool could not stamp SDK $SDK_VERSION; the macOS 26 design may not appear" >&2
fi
cp packaging/macos/Info.plist "$APP/Contents/Info.plist"

# App icon from the SVG (qlmanage renders SVG; iconutil builds the .icns).
ICONSET=build/macos/OpenClone.iconset
rm -rf "$ICONSET" && mkdir -p "$ICONSET"
if qlmanage -t -s 1024 -o build/macos packaging/icons/io.github.openclone.OpenClone.svg >/dev/null 2>&1; then
    SRC=build/macos/io.github.openclone.OpenClone.svg.png
    for s in 16 32 128 256 512; do
        sips -z $s $s "$SRC" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
        sips -z $((s * 2)) $((s * 2)) "$SRC" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/OpenClone.icns" || true
fi

# Ad-hoc signature so Gatekeeper lets a local build run.
codesign --force --sign - "$APP" >/dev/null 2>&1 || true
# Make Launch Services pick up the document types.
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP" 2>/dev/null || true
echo "Built $APP"
