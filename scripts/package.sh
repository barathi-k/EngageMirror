#!/usr/bin/env bash
# Collects airmirror.exe plus every MSYS2 DLL it needs (transitively) into
# dist/, producing a folder that runs on a PC with no MSYS2 and no Bonjour.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="$ROOT/build/airmirror.exe"
DIST="$ROOT/dist"
PREFIX="/c/msys64/ucrt64"

export PATH="$PREFIX/bin:/c/msys64/usr/bin:$PATH"

if [[ ! -f "$EXE" ]]; then
  echo "error: $EXE not found - build first (scripts/build.sh)" >&2
  exit 1
fi

rm -rf "$DIST"
mkdir -p "$DIST"
cp "$EXE" "$DIST/"

if [[ -d "$ROOT/assets" ]]; then
  mkdir -p "$DIST/assets"
  cp "$ROOT/assets/"*.png "$DIST/assets/" 2>/dev/null || true
fi

# ldd walks the whole dependency graph. Keep everything that is NOT a Windows
# system DLL; that covers both MSYS2 libraries and our own vendor/ffmpeg-mini
# build. ldd prints MSYS-rooted paths (/ucrt64/bin/...) that need rewriting.
mapfile -t deps < <(ldd "$EXE" | awk '{print $3}' |
                    grep -viE "^/c/WINDOWS|^/c/Windows" |
                    grep -E "\.dll$" | sort -u || true)

count=0
for dll in "${deps[@]}"; do
  case "$dll" in
    /ucrt64/*) dll="/c/msys64$dll" ;;
    /usr/*)    dll="/c/msys64$dll" ;;
  esac
  [[ -f "$dll" ]] || { echo "  warn: cannot locate $dll" >&2; continue; }
  cp -n "$dll" "$DIST/"
  count=$((count + 1))
done

echo "packaged $(basename "$EXE") + $count DLLs into dist/"
du -sh "$DIST" | awk '{print "total size: " $1}'
echo
echo "Contents:"
ls -1 "$DIST"
