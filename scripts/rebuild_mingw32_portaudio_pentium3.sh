#!/usr/bin/env bash
# Build notes for a Pentium III–safe libportaudio.dll on MSYS2 mingw32 (Win2000/XP retro).
# MSYS2’s stock DLL may use SSE2; rebuild from upstream PortAudio with the same CFLAGS floor as Opus.
set -euo pipefail

export CFLAGS="${CFLAGS:--O2} -march=pentium3 -mtune=pentium3 -mno-sse2 -mfpmath=387"
export CXXFLAGS="${CXXFLAGS:-$CFLAGS}"
export LDFLAGS="${LDFLAGS:-}"

echo "1) Export these flags in the mingw32 shell:"
echo "   export CFLAGS=\"$CFLAGS\""
echo "   export CXXFLAGS=\"$CXXFLAGS\""
echo ""
echo "2) Configure PortAudio for i686-w64-mingw32 with only host APIs you need on XP:"
echo "   ./configure --host=i686-w64-mingw32 --disable-shared --enable-static \\"
echo "     --with-winapi=wmme,directsound   # avoid WASAPI on XP"
echo "   make -j\"$(nproc 2>/dev/null || echo 4)\""
echo ""
echo "3) Link your app against the static .a, or copy the built DLL if you used --enable-shared."
echo "4) Drop libportaudio.dll into vendor/windows-runtime/x86-retro/ so bundle-runtime prefers it."
echo "See docs/specs/windows-legacy-mingw-build.md for the full CPU-tier table."
