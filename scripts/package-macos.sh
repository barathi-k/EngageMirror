#!/usr/bin/env bash
# Builds the app and wraps it in a signed, notarized, stapled DMG. The bundle
# ships as "Engage Mirror.app" (the name shown in Applications); the build
# tree, executable and bundle ID keep the space-free EngageMirror.
#
#   scripts/package-macos.sh               build + sign + DMG + notarize
#   scripts/package-macos.sh --no-notarize build + sign + DMG only
#
# Environment:
#   SIGN_IDENTITY    codesign identity (default: the first "Developer ID
#                    Application" certificate in the keychain)
#   NOTARY_PROFILE   notarytool keychain profile (default: engage-studio); create
#                    one with `xcrun notarytool store-credentials <name> ...`
#
# Output: dist-macos/EngageMirror-<version>.dmg
#
# The DMG bundles whatever device-frame PNGs are in assets/ (see
# assets/README.md). They are not in the repository; run
# scripts/fetch-device-frames.py first if you want iPhone frames.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build-macos"
OUT="$ROOT/dist-macos"
NOTARY_PROFILE="${NOTARY_PROFILE:-engage-studio}"
notarize=1
[[ "${1:-}" == "--no-notarize" ]] && notarize=0

VERSION="$(sed -n 's/^project(EngageMirror VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
BUILT="$BUILD/EngageMirror.app"
DMG="$OUT/EngageMirror-$VERSION.dmg"

if [[ -z "${SIGN_IDENTITY:-}" ]]; then
  SIGN_IDENTITY="$(security find-identity -v -p codesigning |
                   sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -1)"
fi
[[ -n "$SIGN_IDENTITY" ]] || { echo "no Developer ID Application identity found" >&2; exit 1; }

echo ">> dependencies"
[[ -f "$ROOT/vendor/macos-deps/lib/libavcodec.a" ]] || "$ROOT/scripts/build-deps-macos.sh"

echo ">> building EngageMirror $VERSION"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C "$BUILD"

# Renaming the bundle directory before signing keeps the signature covering
# exactly what ships.
APPDIR="$(mktemp -d)"
APP="$APPDIR/Engage Mirror.app"
ditto "$BUILT" "$APP"

frames=$(find "$APP/Contents/Resources/assets" -name '*.png' | wc -l | tr -d ' ')
echo "   $frames device frame(s) bundled"
[[ "$frames" -gt 0 ]] || echo "   warning: no device frames - every device gets the procedural frame" >&2

# Hardened runtime and a secure timestamp: notarization refuses either missing.
# The app is not sandboxed and needs no entitlements: it only listens on the
# local network, decodes video and plays audio.
echo ">> signing with $SIGN_IDENTITY"
codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$APP"
codesign --verify --strict --verbose=2 "$APP"

echo ">> creating $DMG"
mkdir -p "$OUT"
stage="$(mktemp -d)"
trap 'rm -rf "$stage" "$APPDIR"' EXIT
ditto "$APP" "$stage/Engage Mirror.app"
ln -s /Applications "$stage/Applications"
rm -f "$DMG"
hdiutil create -volname "Engage Mirror $VERSION" -srcfolder "$stage" -ov -format UDZO \
  -fs HFS+ "$DMG" >/dev/null
codesign --force --timestamp --sign "$SIGN_IDENTITY" "$DMG"

if [[ $notarize -eq 1 ]]; then
  echo ">> notarizing (profile: $NOTARY_PROFILE) - usually a few minutes"
  xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple "$DMG"
  # What a downloaded copy will be judged by: Gatekeeper, not codesign.
  spctl --assess --type open --context context:primary-signature --verbose=2 "$DMG"
fi

shasum -a 256 "$DMG"
echo ">> done: $DMG"
