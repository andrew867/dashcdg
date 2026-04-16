#!/usr/bin/env bash
# Build static libopus from audio_modules/opus/vendor/opus for MinGW i686,
# with Pentium III–oriented CFLAGS (no SSE2). Requires autotools / make.
# Usage (MSYS2 mingw32):
#   MINGW_ARCH=mingw32 ./scripts/build_opus_mingw32_static.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/audio_modules/opus/vendor/opus"
PREFIX="${OPUS_VENDOR_PREFIX:-$ROOT/build/x86-retro/prefix-opus}"

if [[ ! -f "$SRC/configure.ac" && ! -f "$SRC/configure" ]]; then
  echo "Missing Opus sources at $SRC — run scripts/fetch_opus_portaudio_vendors.sh first." >&2
  exit 1
fi

# In MSYS2 mingw32 env, `gcc` is already i686; override CC when cross-compiling.
export CC="${CC:-gcc}"
CFLAGS="-O2 -march=pentium3 -mtune=pentium3 -mno-sse2 -mfpmath=387 -fno-tree-vectorize -U_FORTIFY_SOURCE"

mkdir -p "$PREFIX"
cd "$SRC"
if [[ ! -f ./configure ]]; then
  ./autogen.sh
fi
./configure \
  --prefix="$PREFIX" \
  --enable-static \
  --disable-shared \
  --disable-doc \
  --disable-extra-programs \
  CFLAGS="$CFLAGS"
make -j"${JOBS:-8}"
make install
echo "Installed static libopus to $PREFIX (lib, include)."
