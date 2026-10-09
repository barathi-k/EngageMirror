#!/usr/bin/env bash
# One-shot build. Assumes MSYS2 is installed at C:\msys64.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="/c/msys64/ucrt64"
export PATH="$PREFIX/bin:/c/msys64/usr/bin:$PATH"

if [[ ! -x "$PREFIX/bin/gcc.exe" ]]; then
  echo "MSYS2 UCRT64 toolchain missing. Install the dependencies with:" >&2
  echo "  /c/msys64/usr/bin/pacman -S --needed --noconfirm \\" >&2
  echo "    mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,openssl,libplist,ffmpeg}" >&2
  exit 1
fi

if [[ ! -f "$ROOT/vendor/UxPlay/lib/raop.c" ]]; then
  echo "Fetching UxPlay sources..."
  git clone --depth 1 https://github.com/FDH2/UxPlay.git "$ROOT/vendor/UxPlay"
fi

cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$ROOT/build"

echo
echo "Built: $ROOT/build/engagemirror.exe"
echo "Run scripts/package.sh to produce a standalone dist/ folder."
