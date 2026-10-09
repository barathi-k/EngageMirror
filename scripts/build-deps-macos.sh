#!/usr/bin/env bash
# Builds EngageMirror's third-party libraries for macOS as static archives:
# OpenSSL (libcrypto only), libplist, and a stripped FFmpeg with VideoToolbox.
#
# Everything is built from pinned, checksummed release tarballs rather than
# taken from Homebrew: Homebrew's builds target the build machine's macOS
# version and are dylibs that would have to be bundled and re-signed, and the
# exact tarballs used here are what a binary release must offer as source
# (they are attached to each GitHub release - see scripts/release-macos.sh).
#
#   scripts/build-deps-macos.sh      -> vendor/macos-deps/{include,lib}
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS="$ROOT/vendor/macos-deps"
SRC="$DEPS/src"           # downloaded tarballs (kept: they are release assets)
WORK="$DEPS/build"
PREFIX="$DEPS"
JOBS="$(sysctl -n hw.ncpu)"

export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-12.0}"
ARCH=arm64

OPENSSL_VER=3.6.5
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VER/openssl-$OPENSSL_VER.tar.gz"
OPENSSL_SHA=a2157c2830efdec3788939b00c9b0638306d3f0bbb76dc4832ee503bb397df98

PLIST_VER=2.8.0
PLIST_URL="https://github.com/libimobiledevice/libplist/releases/download/$PLIST_VER/libplist-$PLIST_VER.tar.bz2"
PLIST_SHA=b1f59f7634c58b2481325a23ff4e3bf51574a42d868cbe466d2b39b04550752a

# Same FFmpeg release the Windows build uses (scripts/build-ffmpeg-mini.sh).
FFMPEG_VER=7.1.1
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz"
FFMPEG_SHA=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1

mkdir -p "$SRC" "$WORK"

fetch() { # url sha -> path of the verified tarball
  local url="$1" sha="$2" out="$SRC/$(basename "$1")"
  if [[ ! -f "$out" ]]; then
    echo ">> downloading $(basename "$out")" >&2
    curl -fL --retry 3 -o "$out.part" "$url"
    mv "$out.part" "$out"
  fi
  echo "$sha  $out" | shasum -a 256 -c - >/dev/null || {
    echo "checksum mismatch for $out" >&2; rm -f "$out"; exit 1; }
  echo "$out"
}

unpack() { # tarball -> fresh source dir under $WORK
  local dir="$WORK/$2"
  rm -rf "$dir"; mkdir -p "$dir"
  tar -xf "$1" -C "$dir" --strip-components 1
  echo "$dir"
}

CFLAGS_COMMON="-arch $ARCH -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -O2"

if [[ ! -f "$PREFIX/lib/libcrypto.a" ]]; then
  d="$(unpack "$(fetch "$OPENSSL_URL" "$OPENSSL_SHA")" openssl)"
  echo ">> building OpenSSL $OPENSSL_VER"
  (cd "$d" &&
   ./Configure darwin64-arm64-cc no-shared no-tests no-docs no-apps no-module \
       --prefix="$PREFIX" --libdir=lib -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET" &&
   make -j"$JOBS" build_libs >/dev/null &&
   make install_dev >/dev/null)
fi

if [[ ! -f "$PREFIX/lib/libplist-2.0.a" ]]; then
  d="$(unpack "$(fetch "$PLIST_URL" "$PLIST_SHA")" libplist)"
  echo ">> building libplist $PLIST_VER"
  (cd "$d" &&
   CFLAGS="$CFLAGS_COMMON" CXXFLAGS="$CFLAGS_COMMON" \
   ./configure --prefix="$PREFIX" --disable-shared --enable-static \
       --without-cython >/dev/null &&
   make -j"$JOBS" >/dev/null && make install >/dev/null)
fi

if [[ ! -f "$PREFIX/lib/libavcodec.a" ]]; then
  d="$(unpack "$(fetch "$FFMPEG_URL" "$FFMPEG_SHA")" ffmpeg)"
  echo ">> building FFmpeg $FFMPEG_VER (decoders only, VideoToolbox)"
  # LGPL configuration (no --enable-gpl): only the decoders AirPlay needs.
  (cd "$d" &&
   ./configure \
     --prefix="$PREFIX" --arch="$ARCH" --cc=clang \
     --extra-cflags="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" \
     --extra-ldflags="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" \
     --disable-everything \
     --enable-static --disable-shared \
     --disable-programs --disable-doc \
     --disable-avdevice --disable-avformat --disable-avfilter --disable-postproc \
     --disable-network --disable-iconv --disable-zlib --disable-bzlib --disable-lzma \
     --disable-sdl2 --disable-securetransport --disable-debug --disable-autodetect \
     --enable-pthreads --enable-videotoolbox \
     --enable-swscale --enable-swresample \
     --enable-decoder=h264,hevc,aac,alac \
     --enable-parser=h264,hevc,aac \
     --enable-hwaccel=h264_videotoolbox,hevc_videotoolbox >/dev/null &&
   make -j"$JOBS" >/dev/null && make install >/dev/null)
fi

echo ">> done: $PREFIX"
ls "$PREFIX/lib"
