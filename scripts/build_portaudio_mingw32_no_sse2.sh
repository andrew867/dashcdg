#!/usr/bin/env bash
# Build static PortAudio from audio_modules/portaudio/vendor/portaudio for MinGW i686,
# with Pentium III–oriented CFLAGS (no SSE2). Use when linking PortAudio instead of
# WinMM on retro / P2–P3-class Windows targets.
# Usage (MSYS2 mingw32):
#   ./scripts/build_portaudio_mingw32_no_sse2.sh
# Optional:
#   PORTAUDIO_VENDOR_PREFIX=/path/to/prefix ./scripts/build_portaudio_mingw32_no_sse2.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/audio_modules/portaudio/vendor/portaudio"
PREFIX="${PORTAUDIO_VENDOR_PREFIX:-$ROOT/build/x86-retro/prefix-portaudio}"
BUILD="$SRC/build-mingw32-no-sse2"

if [[ ! -f "$SRC/CMakeLists.txt" ]]; then
  echo "Missing PortAudio sources at $SRC — run scripts/fetch_opus_portaudio_vendors.sh first." >&2
  exit 1
fi

export CC="${CC:-gcc}"
PA_CFLAGS="-O2 -march=pentium3 -mtune=pentium3 -mno-sse2 -mfpmath=387 -fno-tree-vectorize -U_FORTIFY_SOURCE"

mkdir -p "$PREFIX"
rm -rf "$BUILD"
cmake -S "$SRC" -B "$BUILD" -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_C_FLAGS="$PA_CFLAGS" \
  -DBUILD_SHARED_LIBS=OFF \
  -DPA_BUILD_TESTS=OFF \
  -DPA_BUILD_EXAMPLES=OFF \
  -DPA_USE_WMME=ON \
  -DPA_USE_DSOUND=ON \
  -DPA_USE_WASAPI=ON \
  -DPA_USE_ASIO=OFF

cmake --build "$BUILD" --parallel "${JOBS:-8}"
cmake --install "$BUILD"
echo "Installed static PortAudio to $PREFIX (lib, include). See docs/specs/vendored-opus-portaudio-windows.md."
