#!/usr/bin/env bash
# Optional: shallow-clone libopus and PortAudio into audio_modules/*/vendor/
# (see docs/build/vendored-opus-portaudio-windows.md).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

clone_ref() {
  local name="$1"
  local url="$2"
  local dir="$3"
  if [[ -d "$dir/.git" ]]; then
    echo "skip (exists): $name -> $dir"
    return 0
  fi
  mkdir -p "$(dirname "$dir")"
  echo "clone: $name -> $dir"
  git clone --depth 1 "$url" "$dir"
}

clone_ref "opus (xiph)" "https://github.com/xiph/opus.git" \
  "$ROOT/audio_modules/opus/vendor/opus"

clone_ref "portaudio" "https://github.com/PortAudio/portaudio.git" \
  "$ROOT/audio_modules/portaudio/vendor/portaudio"

echo "Done. Next: see scripts/build_opus_mingw32_static.sh and docs/build/vendored-opus-portaudio-windows.md"
