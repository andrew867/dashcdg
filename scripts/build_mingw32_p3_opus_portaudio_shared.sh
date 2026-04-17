#!/usr/bin/env bash
# Build Pentium III / pre-SSE2–safe shared libopus-0.dll and libportaudio.dll for MinGW i686.
# Installs under build/mingw32-p3-vendor/{opus,portaudio} by default (matches Makefile defaults).
#
# Requires vendor trees from: bash scripts/fetch_opus_portaudio_vendors.sh
# Usage (MSYS2, MINGW32 shell):  ./scripts/build_mingw32_p3_opus_portaudio_shared.sh
#
# Optional: OPUS_VENDOR_PREFIX PORTAUDIO_VENDOR_PREFIX JOBS
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

export CC="${CC:-gcc}"
export CXX="${CXX:-g++}"
# Pentium III class: no SSE2 in third-party code (match dashcdg -march=pentium3 objects).
# Pentium MMX and older: use -march=pentium-mmx in a custom build if you must; Opus is not tuned for that here.
P3_CFLAGS="-O2 -march=pentium3 -mtune=pentium3 -mno-sse2 -mfpmath=387 -fno-tree-vectorize -fno-tree-slp-vectorize -U_FORTIFY_SOURCE"

OPUS_SRC="$ROOT/audio_modules/opus/vendor/opus"
PA_SRC="$ROOT/audio_modules/portaudio/vendor/portaudio"
OPUS_PREFIX="${OPUS_VENDOR_PREFIX:-$ROOT/build/mingw32-p3-vendor/opus}"
PA_PREFIX="${PORTAUDIO_VENDOR_PREFIX:-$ROOT/build/mingw32-p3-vendor/portaudio}"

if [[ ! -f "$OPUS_SRC/configure.ac" && ! -f "$OPUS_SRC/configure" ]]; then
  echo "[p3-vendor] Missing Opus sources at $OPUS_SRC" >&2
  echo "  Run:  bash scripts/fetch_opus_portaudio_vendors.sh" >&2
  exit 1
fi
if [[ ! -f "$PA_SRC/CMakeLists.txt" ]]; then
  echo "[p3-vendor] Missing PortAudio sources at $PA_SRC" >&2
  echo "  Run:  bash scripts/fetch_opus_portaudio_vendors.sh" >&2
  exit 1
fi

echo "[p3-vendor] Building shared libopus -> $OPUS_PREFIX"
mkdir -p "$OPUS_PREFIX"
cd "$OPUS_SRC"
if [[ ! -f ./configure ]]; then
  ./autogen.sh
fi
# Shared + static: package copies bin/libopus-0.dll
./configure \
  --prefix="$OPUS_PREFIX" \
  --enable-shared \
  --enable-static \
  --disable-doc \
  --disable-extra-programs \
  CFLAGS="$P3_CFLAGS" \
  CXXFLAGS="$P3_CFLAGS"
make -j"${JOBS:-8}"
make install

cd "$ROOT"
echo "[p3-vendor] Building shared PortAudio -> $PA_PREFIX"
PA_BUILD="$PA_SRC/build-dashcdg-mingw32-p3-shared"
rm -rf "$PA_BUILD"
# WASAPI off: friendlier to Windows 2000 / older hosts; WMME + DSOUND remain.
cmake -S "$PA_SRC" -B "$PA_BUILD" -G "MinGW Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PA_PREFIX" \
  -DCMAKE_C_FLAGS="$P3_CFLAGS" \
  -DBUILD_SHARED_LIBS=ON \
  -DPA_BUILD_TESTS=OFF \
  -DPA_BUILD_EXAMPLES=OFF \
  -DPA_USE_WMME=ON \
  -DPA_USE_DSOUND=ON \
  -DPA_USE_WASAPI=OFF \
  -DPA_USE_ASIO=OFF

cmake --build "$PA_BUILD" --parallel "${JOBS:-8}"
cmake --install "$PA_BUILD"

if [[ ! -f "$OPUS_PREFIX/bin/libopus-0.dll" && ! -f "$OPUS_PREFIX/bin/libopus.dll" ]]; then
  echo "[p3-vendor] warning: expected libopus-0.dll under $OPUS_PREFIX/bin — check configure output" >&2
fi
if [[ ! -f "$PA_PREFIX/bin/libportaudio.dll" ]]; then
  echo "[p3-vendor] error: missing $PA_PREFIX/bin/libportaudio.dll" >&2
  exit 1
fi

echo "[p3-vendor] OK: Opus + PortAudio shared libraries installed."
echo "  Opus:     $OPUS_PREFIX/bin/"
echo "  PortAudio: $PA_PREFIX/bin/"
