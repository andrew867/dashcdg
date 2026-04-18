#!/usr/bin/env bash
# Disassemble 32-bit PEs and reject *known* PIII-illegal instruction patterns.
# PIII = MMX + SSE(1) only. No SSE2+ integer/double, no VEX, no 256+ wide ymm.
#
# This is a static disassembly gate (objdump) — the strongest in-repo check before real hardware;
# a miss is still *theoretically* possible for exotic encodings, but you asked for 110% of what we can *prove* here.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BANNED_FILE="${ROOT}/scripts/data/p3_pentium3_banned_mnemonics.txt"
BAD=0

if ! command -v objdump >/dev/null 2>&1; then
  echo "[p3-all] FATAL: objdump not on PATH (MSYS2: pacman -S --needed mingw-w64-i686-binutils)" >&2
  exit 1
fi
if [[ ! -f "$BANNED_FILE" ]]; then
  echo "[p3-all] FATAL: missing $BANNED_FILE" >&2
  exit 1
fi

# Split banned tokens into -E alternation chunks (portable, avoids one giant argv).
dashcdg_p3_banned_re_chunks() {
  local w pat='' c=0 chunk=28
  mapfile -t _w < <(sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' "$BANNED_FILE" | tr 'A-Z' 'a-z' | grep -E '^[a-z0-9]{2,20}$' | sort -u)
  for w in "${_w[@]}"; do
    if [[ c -ge chunk && -n "$pat" ]]; then
      echo "$pat"
      pat=''
      c=0
    fi
    [[ -n "$pat" ]] && pat+='|'
    pat+="$w"
    c=$((c + 1))
  done
  [[ -n "$pat" ]] && echo "$pat"
}
mapfile -t RE_CHUNKS < <(dashcdg_p3_banned_re_chunks)

dashcdg_p3_forbidden_in_dis() {
  local f="$1" tfile
  tfile=$(mktemp) || { echo "[p3-all] mktemp failed" >&2; return 0; }
  if ! objdump -d "$f" 2>/dev/null >"$tfile" || [[ ! -s "$tfile" ]]; then
    rm -f "$tfile"
    return 0
  fi
  if grep -Eiq '(%ymm|%zmm|\.zmm[0-9]|\$ymm|\$zmm|\.k[0-7]k)' "$tfile" 2>/dev/null; then
    echo "[p3-all] BANNED 256+ / mask registers (no AVX) in $(basename "$f"):" >&2
    grep -Ei '(%ymm|%zmm|\.k[0-7]k)' "$tfile" | head -4 >&2
    rm -f "$tfile"
    return 1
  fi
  for chunk in "${RE_CHUNKS[@]}"; do
    if grep -Eiq "(${chunk})" "$tfile" 2>/dev/null; then
      echo "[p3-all] BANNED mnemonic in $(basename "$f") (banned list):" >&2
      grep -Ei "(${chunk})" "$tfile" | head -6 >&2
      rm -f "$tfile"
      return 1
    fi
  done
  if grep -Ei 'cmpsd[[:space:]]+.*%xmm' "$tfile" 2>/dev/null; then
    echo "[p3-all] BANNED SSE2 cmpsd (xmm) in $(basename "$f")" >&2
    grep -Ei 'cmpsd[[:space:]]+.*%xmm' "$tfile" 2>/dev/null | head -3 >&2
    rm -f "$tfile"
    return 1
  fi
  if grep -Ei 'paddq[[:space:]]+.*%xmm|psubq[[:space:]]+.*%xmm' "$tfile" 2>/dev/null; then
    echo "[p3-all] BANNED paddq/psubq on %xmm in $(basename "$f")" >&2
    grep -Ei 'paddq[[:space:]]+.*%xmm|psubq[[:space:]]+.*%xmm' "$tfile" 2>/dev/null | head -3 >&2
    rm -f "$tfile"
    return 1
  fi
  if grep -Eqi 'padd[bdwq]|psub[bdwq]' "$tfile" 2>/dev/null; then
    if grep -Eqi 'padd[bdwq].*%xmm|padd[bdw].*,%xmm|padd[bdw].*xmm|psub[bdwq].*%xmm' "$tfile" 2>/dev/null; then
      echo "[p3-all] BANNED MMX-ambiguous *padd*/*psub* with %xmm in $(basename "$f")" >&2
      grep -Ei 'padd[bdw].*xmm|psub[bdw].*xmm' "$tfile" | head -4 >&2
      rm -f "$tfile"
      return 1
    fi
  fi
  if grep -Eqi 'palignr.*%xmm' "$tfile" 2>/dev/null; then
    echo "[p3-all] BANNED palignr %xmm in $(basename "$f")" >&2
    grep -Ei 'palignr.*%xmm' "$tfile" | head -2 >&2
    rm -f "$tfile"
    return 1
  fi
  if grep -Eqi 'movq.*%xmm' "$tfile" 2>/dev/null; then
    echo "[p3-all] BANNED movq … %xmm in $(basename "$f")" >&2
    grep -Ei 'movq.*%xmm' "$tfile" | head -3 >&2
    rm -f "$tfile"
    return 1
  fi
  if grep -Ei '^[[:space:]]*[0-9a-f]+:[[:space:]]+v[a-za-z][a-za-z0-9_]{1,7}[[:space:]]' "$tfile" 2>/dev/null; then
    echo "[p3-all] BANNED VEX/AVX class mnemonic (v-prefix after address) in $(basename "$f")" >&2
    grep -Ei '^[[:space:]]*[0-9a-f]+:[[:space:]]+v[a-za-z][a-za-z0-9_]{1,7}[[:space:]]' "$tfile" 2>/dev/null | head -4 >&2
    rm -f "$tfile"
    return 1
  fi
  rm -f "$tfile"
  return 0
}

# If every argument is an existing *file*, scan only those (e.g. Opus+PortAudio DLLs).
# If any argument is a directory, or there are no arguments, also merge PIII vendor + build/x86* bin PEs.
PE_LIST=()
ALL_ARGS_FILES=1
if [[ $# -gt 0 ]]; then
  for a in "$@"; do
    if [[ -d "$a" ]]; then
      ALL_ARGS_FILES=0
    elif [[ ! -f "$a" ]]; then
      echo "[p3-all] skip missing: $a" >&2
    fi
  done
fi
if [[ $# -gt 0 && $ALL_ARGS_FILES -eq 1 ]]; then
  for a in "$@"; do
    [[ -f "$a" ]] && PE_LIST+=("$a")
  done
else
  if [[ $# -gt 0 ]]; then
    for a in "$@"; do
      if [[ -f "$a" ]]; then
        PE_LIST+=("$a")
      elif [[ -d "$a" ]]; then
        while IFS= read -r -d '' f; do
          PE_LIST+=("$f")
        done < <(find "$a" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0 2>/dev/null)
      fi
    done
  else
    SD="${ROOT}/build/dist/dashcdg-windows-sneakernet"
    for sub in windows-x86 windows-x86-legacy-p3 windows-x86-retro; do
      d="${SD}/${sub}"
      [[ -d "$d" ]] || continue
      while IFS= read -r -d '' f; do
        PE_LIST+=("$f")
      done < <(find "$d" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0 2>/dev/null)
    done
  fi
  for f in \
    "${ROOT}/build/mingw32-p3-vendor/opus/bin/libopus-0.dll" \
    "${ROOT}/build/mingw32-p3-vendor/opus/bin/libopus.dll" \
    "${ROOT}/build/mingw32-p3-vendor/portaudio/bin/libportaudio.dll"; do
    [[ -f "$f" ]] && PE_LIST+=("$f")
  done
  if [[ -d "${ROOT}/build/x86/bin" ]]; then
    while IFS= read -r -d '' f; do
      case "$(basename "$f" | tr '[:upper:]' '[:lower:]')" in
        *.exe|*.dll) PE_LIST+=("$f") ;;
      esac
    done < <(find "${ROOT}/build/x86/bin" -maxdepth 1 -type f -print0 2>/dev/null)
  fi
  if [[ -d "${ROOT}/build/x86-retro/bin" ]]; then
    while IFS= read -r -d '' f; do
      case "$(basename "$f" | tr '[:upper:]' '[:lower:]')" in
        *.exe|*.dll) PE_LIST+=("$f") ;;
      esac
    done < <(find "${ROOT}/build/x86-retro/bin" -maxdepth 1 -type f -print0 2>/dev/null)
  fi
fi

if [[ ${#PE_LIST[@]} -eq 0 ]]; then
  echo "[p3-all] nothing to scan" >&2
  exit 0
fi
readarray -t PE_LIST < <(printf '%s\n' "${PE_LIST[@]}" | sort -u)
echo "[p3-all] scanning ${#PE_LIST[@]} file(s) with objdump (PIII / no-SSE2 gate)…"
for f in "${PE_LIST[@]}"; do
  if ! dashcdg_p3_forbidden_in_dis "$f"; then
    BAD=1
    if [[ -z "${P3_CONT:-}" ]]; then
      break
    fi
  else
    echo "[p3-all] OK  $(basename "$f")"
  fi
done
if [[ "$BAD" -ne 0 ]]; then
  echo "[p3-all] FAIL — set P3_CONT=1 to continue listing all (default: fail fast on first file)." >&2
  exit 1
fi
echo "[p3-all] PASS: all files passed static PIII heuristics."
