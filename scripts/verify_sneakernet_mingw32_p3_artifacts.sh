#!/usr/bin/env bash
# Heuristic: objdump of sneakernet 32-bit EXEs+DLLs must not show common SSE2-only x86-64 / XMM patterns.
# Run on MSYS2 after: bash scripts/build_windows_sneakernet_dist.sh
# Usage: bash scripts/verify_sneakernet_mingw32_p3_artifacts.sh [path_to_dashcdg-windows-sneakernet]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SD="${1:-$ROOT/build/dist/dashcdg-windows-sneakernet}"

# Keep in sync with scripts/verify_mingw32_p3_codec_dlls.sh
SSE2_RE='movdqa|movdqu|movapd|movupd|movlpd|movhpd|mulpd|addpd|subpd|minpd|maxpd|cmpeqpd|andpd|orpd|xorpd|shufpd|paddq|pmuludq|psadbw|punpcklqdq|punpckhqdq'

if ! command -v objdump >/dev/null 2>&1; then
  echo "[p3-sneakernet] objdump not on PATH, skip" >&2
  exit 0
fi

bad=0
for sub in windows-x86 windows-x86-legacy-p3 windows-x86-retro; do
  d="${SD}/${sub}"
  if [[ ! -d "$d" ]]; then
    continue
  fi
  shopt -s nullglob
  for f in "$d"/*.{exe,dll}; do
    b="$(basename "$f")"
    # x64 64-bit PE can legitimately use xmm in ways we don't test here; we only check i686 tree.
    if objdump -d "$f" 2>/dev/null | grep -qiE "$SSE2_RE"; then
      echo "[p3-sneakernet] FAIL: possible SSE2-class / wide XMM patterns in $sub/$b" >&2
      objdump -d "$f" 2>/dev/null | grep -iE "$SSE2_RE" | head -5 >&2
      bad=1
    else
      echo "[p3-sneakernet] OK: $sub/$b"
    fi
  done
  shopt -u nullglob
done

if [[ "$bad" -ne 0 ]]; then
  echo "[p3-sneakernet] Scan failed. If this is a false positive (e.g. SSE1 movaps), adjust script." >&2
  exit 1
fi
echo "[p3-sneakernet] all mingw32 artifacts passed heuristic disassembly check."
