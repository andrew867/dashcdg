#!/usr/bin/env bash
# Optional: clone codec trees into audio_modules/*/vendor/ (see audio_modules/README.md).
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
  echo "clone: $name"
  git clone --depth 1 "$url" "$dir"
}

clone_ref "codec-amr" "https://github.com/pschatzmann/codec-amr.git" \
  "$ROOT/audio_modules/amr/vendor/codec-amr"

clone_ref "evrcc" "https://github.com/maolin-cdzl/evrcc.git" \
  "$ROOT/audio_modules/evr/vendor/evrcc"

clone_ref "celp13k" "https://github.com/RupW/celp13k.git" \
  "$ROOT/audio_modules/qcelp/vendor/celp13k"

clone_ref "sbc" "https://git.kernel.org/pub/scm/bluetooth/sbc.git" \
  "$ROOT/audio_modules/bt_sbc/vendor/sbc"

echo "Done."
