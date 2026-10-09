#!/usr/bin/env bash
# Builds a stripped FFmpeg containing only what AirMirror decodes.
#
# MSYS2's stock ffmpeg package pulls in every encoder plus pango/cairo/librsvg,
# which balloons a redistributable build to ~115 MB. Everything we actually
# need is four decoders and the D3D11VA hwaccel, which comes to a few MB.
#
# Run from an MSYS2 UCRT64 shell (or via scripts/build-ffmpeg-mini.cmd).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="$ROOT/vendor/ffmpeg-mini"
SRC="$ROOT/vendor/ffmpeg-src"
VERSION="${FFMPEG_VERSION:-n7.1.1}"

export PATH="/ucrt64/bin:/usr/bin:$PATH"

echo ">> ensuring build tools"
pacman -S --needed --noconfirm make diffutils nasm git >/dev/null

if [[ ! -d "$SRC" ]]; then
  echo ">> fetching FFmpeg $VERSION"
  git clone --depth 1 --branch "$VERSION" https://github.com/FFmpeg/FFmpeg.git "$SRC"
fi

mkdir -p "$SRC/build-mini"
cd "$SRC/build-mini"

if [[ ! -f config.h ]]; then
  echo ">> configuring"
  ../configure \
    --prefix="$PREFIX" \
    --disable-everything \
    --disable-static --enable-shared \
    --disable-programs --disable-doc --disable-htmlpages --disable-manpages \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-postproc \
    --disable-network --disable-iconv --disable-zlib --disable-bzlib --disable-lzma \
    --disable-sdl2 --disable-schannel --disable-debug --disable-autodetect \
    --enable-swscale --enable-swresample \
    --enable-d3d11va --enable-dxva2 \
    --enable-decoder=h264,hevc,aac,alac \
    --enable-parser=h264,hevc,aac \
    --enable-hwaccel=h264_d3d11va,h264_d3d11va2,h264_dxva2 \
    --enable-hwaccel=hevc_d3d11va,hevc_d3d11va2,hevc_dxva2
fi

echo ">> building"
make -j"$(nproc)"
make install

echo
echo ">> done. Installed to $PREFIX"
du -sh "$PREFIX/bin" 2>/dev/null | awk '{print "   runtime DLL size: " $1}'
ls -1 "$PREFIX/bin"
