#!/bin/sh
# Builds Restricto.app (Swift + AppKit) with SwiftPM. Run on macOS 26+ with
# Xcode 26 or its Command Line Tools installed.
#   scripts/build_macos_app.sh            -> build/macos/Restricto.app
set -eu
cd "$(dirname "$0")/.."

swift build -c release --product Restricto
swift build -c release --product RestrictoQuickLook
BIN_DIR="$(swift build -c release --show-bin-path)"
BIN="$BIN_DIR/Restricto"

APP=build/macos/Restricto.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/Restricto"
# SwiftPM can record the deployment target (26.0) as the SDK version. AppKit
# keys the newest design (floating glass sidebar, glass toolbar) off the SDK
# version, so stamp the real one in; the minimum system version stays 26.0.
SDK_VERSION="$(xcrun --show-sdk-version 2>/dev/null || true)"
if [ -n "$SDK_VERSION" ] && command -v vtool >/dev/null 2>&1; then
    vtool -set-build-version macos 26.0 "$SDK_VERSION" -replace \
        -output "$APP/Contents/MacOS/Restricto" "$BIN" 2>/dev/null ||
        echo "warning: vtool could not stamp SDK $SDK_VERSION; the macOS 26 design may not appear" >&2
fi
cp packaging/macos/Info.plist "$APP/Contents/Info.plist"

# The Quick Look preview (Space on a sequence file in Finder), stamped like the app.
APPEX="$APP/Contents/PlugIns/RestrictoQuickLook.appex"
mkdir -p "$APPEX/Contents/MacOS"
cp packaging/macos/QuickLook-Info.plist "$APPEX/Contents/Info.plist"
cp "$BIN_DIR/RestrictoQuickLook" "$APPEX/Contents/MacOS/RestrictoQuickLook"
if [ -n "$SDK_VERSION" ] && command -v vtool >/dev/null 2>&1; then
    vtool -set-build-version macos 26.0 "$SDK_VERSION" -replace \
        -output "$APPEX/Contents/MacOS/RestrictoQuickLook" "$BIN_DIR/RestrictoQuickLook" 2>/dev/null || true
fi

# App icon from the 1024 px artwork (already inset to the macOS icon grid);
# sips scales it and iconutil builds the .icns.
ICONSET=build/macos/Restricto.iconset
rm -rf "$ICONSET" && mkdir -p "$ICONSET"
SRC=packaging/macos/AppIcon.png
for s in 16 32 128 256 512; do
    sips -z $s $s "$SRC" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) "$SRC" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/Restricto.icns"

# Ad-hoc signatures so Gatekeeper lets a local build run; the extension first,
# sandboxed, as app extensions must be.
codesign --force --sign - --entitlements packaging/macos/QuickLook.entitlements "$APPEX" >/dev/null 2>&1 || true
codesign --force --sign - "$APP" >/dev/null 2>&1 || true
# Make Launch Services pick up the document types.
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP" 2>/dev/null || true
echo "Built $APP"
