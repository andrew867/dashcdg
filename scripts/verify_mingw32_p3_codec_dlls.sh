#!/usr/bin/env bash
# Opus + PortAudio DLLs only (fast); uses the same disassembly rules as verify_p3_pe_pentium3.sh.
# Usage: bash scripts/verify_mingw32_p3_codec_dlls.sh [opus.dll] [portaudio.dll]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OPUS_DLL="${1:-$ROOT/build/mingw32-p3-vendor/opus/bin/libopus-0.dll}"
if [[ ! -f "$OPUS_DLL" ]]; then
  OPUS_DLL="${1:-$ROOT/build/mingw32-p3-vendor/opus/bin/libopus.dll}"
fi
PA_DLL="${2:-$ROOT/build/mingw32-p3-vendor/portaudio/bin/libportaudio.dll}"
exec bash "$ROOT/scripts/verify_p3_pe_pentium3.sh" "$OPUS_DLL" "$PA_DLL"
