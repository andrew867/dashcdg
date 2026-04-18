#!/usr/bin/env bash
# Fails if disassembly of PIII vendor codec DLLs shows common SSE2-only mnemonics
# (heuristic; build with -march=pentium3 -mno-sse2 in build_mingw32_p3_opus_portaudio_shared.sh).
# Usage: bash scripts/verify_mingw32_p3_codec_dlls.sh [opus.dll] [portaudio.dll]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OPUS_DLL="${1:-$ROOT/build/mingw32-p3-vendor/opus/bin/libopus-0.dll}"
if [[ ! -f "$OPUS_DLL" ]]; then
  OPUS_DLL="${1:-$ROOT/build/mingw32-p3-vendor/opus/bin/libopus.dll}"
fi
PA_DLL="${2:-$ROOT/build/mingw32-p3-vendor/portaudio/bin/libportaudio.dll}"

if ! command -v objdump >/dev/null 2>&1; then
  echo "[p3-verify] objdump not found — install mingw-w64-binutils or use MSYS2 MinGW toolchain on PATH." >&2
  exit 1
fi

for f in "$OPUS_DLL" "$PA_DLL"; do
  if [[ ! -f "$f" ]]; then
    echo "[p3-verify] missing: $f" >&2
    exit 1
  fi
done

# SSE2-introduced SIMD patterns (heuristic subset). PIII-safe builds may still use SSE1 (movaps, xorps).
SSE2_RE='movdqa|movdqu|movapd|movupd|movlpd|movhpd|mulpd|addpd|subpd|minpd|maxpd|cmpeqpd|andpd|orpd|xorpd|shufpd|paddq|pmuludq|psadbw|punpcklqdq|punpckhqdq'

bad=0
for f in "$OPUS_DLL" "$PA_DLL"; do
  echo "[p3-verify] scanning $(basename "$f") ..."
  if objdump -d "$f" 2>/dev/null | grep -qiE "$SSE2_RE"; then
    echo "[p3-verify] FAIL: possible SSE2-class mnemonics in $f (sample):" >&2
    objdump -d "$f" 2>/dev/null | grep -iE "$SSE2_RE" | head -15 >&2
    bad=1
  else
    echo "[p3-verify] OK (no SSE2 heuristic hits): $f"
  fi
done

if [[ "$bad" -ne 0 ]]; then
  exit 1
fi
