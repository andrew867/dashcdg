#!/usr/bin/env bash
# Clone optional audio codec upstreams into audio_modules/*/vendor/
# (gitignored). Review audio_modules/NOTICES.md before product use.
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
  "$ROOT/audio_modules/amr_pschatzmann/vendor/codec-amr"

clone_ref "gpu.evrc" "https://github.com/arulk77/gpu.evrc.git" \
  "$ROOT/audio_modules/evrc_arulk77/vendor/gpu.evrc"

clone_ref "evrcc" "https://github.com/maolin-cdzl/evrcc.git" \
  "$ROOT/audio_modules/evrc_maolin/vendor/evrcc"

clone_ref "celp13k" "https://github.com/RupW/celp13k.git" \
  "$ROOT/audio_modules/qcelp_rupw/vendor/celp13k"

clone_ref "sbc" "https://git.kernel.org/pub/scm/bluetooth/sbc.git" \
  "$ROOT/audio_modules/bluetooth_sbc_kernel/vendor/sbc"

echo "Done. Vendored trees are under audio_modules/*/vendor/ (see .gitignore)."
