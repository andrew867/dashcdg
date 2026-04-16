#!/usr/bin/env bash
# Recommend MinGW32 flags for Opus (and other deps) when targeting Pentium III / no SSE2.
# Optional: pass a built DLL path to scan for obvious SSE2 opcodes.
set -euo pipefail

export CFLAGS="${CFLAGS:--O2} -march=pentium3 -mtune=pentium3 -mno-sse2 -mfpmath=387"
export CXXFLAGS="${CXXFLAGS:-$CFLAGS}"
export LDFLAGS="${LDFLAGS:-}"

echo "Use these with MSYS2 mingw32 when rebuilding libopus (and keep the same floor for PortAudio if needed):"
echo "  export CFLAGS=\"$CFLAGS\""
echo "  export CXXFLAGS=\"$CXXFLAGS\""
echo ""
echo "Then build from the mingw-w64-mingw32-opus PKGBUILD or opus autotools tarball with --host=i686-w64-mingw32."
echo "Copy libopus-0.dll beside the dashcdg EXE and re-run sneakernet packaging."
echo "Full context: docs/specs/windows-legacy-mingw-build.md (CPU tiers + Opus section)."

if [ "${1:-}" != "" ]; then
    if ! command -v objdump >/dev/null 2>&1; then
        echo "objdump not found; skip DLL scan." >&2
        exit 0
    fi
    echo ""
    echo "Scanning $1 for movq-to-xmm / common SSE2 patterns (first hits):"
    if objdump -d "$1" 2>/dev/null | grep -E 'movq[[:space:]].*xmm|pslld|paddq' | head -n 20; then
        echo "^^ If you see many matches on a PIII floor build, the DLL is still SSE2-heavy."
    else
        echo "(no obvious SSE2 patterns in first pass — still test on hardware)"
    fi
fi
