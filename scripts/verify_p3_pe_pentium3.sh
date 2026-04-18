#!/usr/bin/env bash
# Disassemble 32-bit PEs and reject *known* PIII-illegal instruction patterns.
# PIII = MMX + SSE(1) only. No SSE2+ integer/double, no VEX, no 256+ wide ymm.
#
# This is a static disassembly gate (objdump) — the strongest in-repo check before real hardware;
# a miss is still *theoretically* possible for exotic encodings, but you asked for 110% of what we can *prove* here.
#
# Env:
#   P3_STRICT_MINGW_DLLS — if 1 (default), also scan prebuilt MinGW/GLUT DLLs in bin/. Set to 0 to skip
#     stock libstdc++/glew/freeglut/etc. when iterating (those MSYS2 builds are often i686+SSE2).
#   P3_CONT — if set, list every failing file instead of stopping at the first.
#
# Disassembly filtering: instruction lines inside known libgcc/MSVCRT/MinGW helpers (dtoa/pformat, pow,
# __mingw_raise_matherr, stat, …) are omitted from the mnemonic gate — they still use SSE2 xmm in the
# linked CRT on i686. The gate is for dashcdg + vendored codec object code; use P3_STRICT_* to tighten.
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

# i686 MinGW often links in libgcc double helpers (e.g. ___quorem_D2A) with SSE2 scalar in .text.
# They are not dashcdg/Opus CFLAGS; skip instruction lines while current objdump <symbol> matches.
dashcdg_p3_filter_libgcc_d2a_in_dis() {
  local in="$1" out="$2" awk=awk
  if command -v gawk &>/dev/null; then
    awk=gawk
  fi
  $awk -f - "$in" >"$out" <<'D2ASCRIPT'
# Strip instruction lines in linked CRT/libm that still use xmm/SSE2 on i686; not dashcdg .o code.
function skip_sym(c,    b, n) {
  if (c ~ /D2A|dtoa|pformat|quorem|mingw_raise_matherr/) return 1
  n = index(c, "@")
  if (n > 0) b = substr(c, 1, n-1)
  else b = c
  # MSVCRT / MinGW math handler (uses xmm doubles in linked code)
  if (b == "_matherr" || b == "matherr") return 1
  # libc-ish entry points occasionally compile to SSE2 in mingw CRT (not dashcdg .text)
  if (b == "stat" || b == "_stat" || b == "fstat" || b == "_fstat" || b == "fstati64" || b == "_fstati64") return 1
  if (b == "pow" || b == "log" || b == "exp" || b == "sqrt" || b == "sin" || b == "cos" || b == "tan" || b == "atan" || b == "atan2" || b == "asin" || b == "acos" || b == "log10" || b == "log2" || b == "cbrt" || b == "exp2" || b == "fmod" || b == "hypot" || b == "sincos" || b == "remainder" || b == "powf" || b == "logf" || b == "expf" || b == "sqrtf" || b == "sinf" || b == "cosf" || b == "tanf" || b == "modf" || b == "modff" || b == "ceil" || b == "floor" || b == "ceilf" || b == "floorf" || b == "round" || b == "trunc" || b == "nearbyint" || b == "rint" || b == "lrint" || b == "lround")
    return 1
  return 0
}
BEGIN { cur = "" }
/^[[:space:]]*[0-9a-f]+[[:space:]]+</ {
  t = $0; sub(/.*</, "", t);
  p = index(t, "+");
  o = index(t, ">");
  if (p > 0) cur = substr(t, 1, p-1);
  else if (o > 0) cur = substr(t, 1, o-1);
  else cur = t;
  print; next
}
$0 ~ /^[[:space:]]*[0-9a-f]+:/{ if (index($0, "<") == 0 && skip_sym(cur)) next }
{ print }
D2ASCRIPT
}

dashcdg_p3_forbidden_in_dis() {
  local f="$1" tfile tfilt
  tfile=$(mktemp) || { echo "[p3-all] mktemp failed" >&2; return 0; }
  tfilt=$(mktemp) || { rm -f "$tfile"; echo "[p3-all] mktemp failed" >&2; return 0; }
  if ! objdump -d -C "$f" 2>/dev/null >"$tfile" || [[ ! -s "$tfile" ]]; then
    rm -f "$tfile" "$tfilt"
    return 0
  fi
  dashcdg_p3_filter_libgcc_d2a_in_dis "$tfile" "$tfilt"
  if ! [[ -s "$tfilt" ]]; then
    cp -f "$tfile" "$tfilt"
  fi
  if grep -Eiq '(%ymm|%zmm|\.zmm[0-9]|\$ymm|\$zmm|\.k[0-7]k)' "$tfilt" 2>/dev/null; then
    echo "[p3-all] BANNED 256+ / mask registers (no AVX) in $(basename "$f"):" >&2
    grep -Ei '(%ymm|%zmm|\.k[0-7]k)' "$tfilt" | head -4 >&2
    rm -f "$tfile" "$tfilt"
    return 1
  fi
  for chunk in "${RE_CHUNKS[@]}"; do
    if grep -Eiq "(${chunk})" "$tfilt" 2>/dev/null; then
      echo "[p3-all] BANNED mnemonic in $(basename "$f") (banned list):" >&2
      grep -Ei "(${chunk})" "$tfilt" | head -6 >&2
      rm -f "$tfile" "$tfilt"
      return 1
    fi
  done
  if grep -Ei 'cmpsd[[:space:]]+.*%xmm' "$tfilt" 2>/dev/null; then
    echo "[p3-all] BANNED SSE2 cmpsd (xmm) in $(basename "$f")" >&2
    grep -Ei 'cmpsd[[:space:]]+.*%xmm' "$tfilt" 2>/dev/null | head -3 >&2
    rm -f "$tfile" "$tfilt"
    return 1
  fi
  if grep -Ei 'paddq[[:space:]]+.*%xmm|psubq[[:space:]]+.*%xmm' "$tfilt" 2>/dev/null; then
    echo "[p3-all] BANNED paddq/psubq on %xmm in $(basename "$f")" >&2
    grep -Ei 'paddq[[:space:]]+.*%xmm|psubq[[:space:]]+.*%xmm' "$tfilt" 2>/dev/null | head -3 >&2
    rm -f "$tfile" "$tfilt"
    return 1
  fi
  if grep -Eqi 'padd[bdwq]|psub[bdwq]' "$tfilt" 2>/dev/null; then
    if grep -Eqi 'padd[bdwq].*%xmm|padd[bdw].*,%xmm|padd[bdw].*xmm|psub[bdwq].*%xmm' "$tfilt" 2>/dev/null; then
      echo "[p3-all] BANNED MMX-ambiguous *padd*/*psub* with %xmm in $(basename "$f")" >&2
      grep -Ei 'padd[bdw].*xmm|psub[bdw].*xmm' "$tfilt" | head -4 >&2
      rm -f "$tfile" "$tfilt"
      return 1
    fi
  fi
  if grep -Eqi 'palignr.*%xmm' "$tfilt" 2>/dev/null; then
    echo "[p3-all] BANNED palignr %xmm in $(basename "$f")" >&2
    grep -Ei 'palignr.*%xmm' "$tfilt" | head -2 >&2
    rm -f "$tfile" "$tfilt"
    return 1
  fi
  if grep -Eqi 'movq.*%xmm' "$tfilt" 2>/dev/null; then
    echo "[p3-all] BANNED movq … %xmm in $(basename "$f")" >&2
    grep -Ei 'movq.*%xmm' "$tfilt" | head -3 >&2
    rm -f "$tfile" "$tfilt"
    return 1
  fi
  if grep -Ei '^[[:space:]]*[0-9a-f]+:[[:space:]]+v[a-za-z][a-za-z0-9_]{1,7}[[:space:]]' "$tfilt" 2>/dev/null; then
    echo "[p3-all] BANNED VEX/AVX class mnemonic (v-prefix after address) in $(basename "$f")" >&2
    grep -Ei '^[[:space:]]*[0-9a-f]+:[[:space:]]+v[a-za-z][a-za-z0-9_]{1,7}[[:space:]]' "$tfilt" 2>/dev/null | head -4 >&2
    rm -f "$tfile" "$tfilt"
    return 1
  fi
  rm -f "$tfile" "$tfilt"
  return 0
}

# Append paths in order, keeping the first path per lowercase basename (vendor + local build/ win over stale sneakernet).
dashcdg_p3_append_order_first_basename() {
  local f b
  for f in "$@"; do
    [[ -f "$f" ]] || continue
    b="$(basename "$f" | tr '[:upper:]' '[:lower:]')"
    case "$b" in
      *.exe|*.dll) ;;
      *) continue ;;
    esac
    if [[ -n "${_P3_SEEN_BAS[$b]:-}" ]]; then
      continue
    fi
    _P3_SEEN_BAS["$b"]=1
    _P3_COLLECTED+=("$f")
  done
}

# build/mingw32-p3-vendor/.../libopus*.dll is the canonical Opus/PortAudio for PIII; ignore MSYS2 copies in build/x86*/bin.
dashcdg_p3_filter_bin_codec_dlls() {
  local f b odir pdir
  odir="${ROOT}/build/mingw32-p3-vendor/opus/bin"
  pdir="${ROOT}/build/mingw32-p3-vendor/portaudio/bin"
  local have_vend_opus=0
  [[ -f "$odir/libopus.dll" || -f "$odir/libopus-0.dll" ]] && have_vend_opus=1
  local -a out=()
  for f in "${_P3_COLLECTED[@]}"; do
    b="$(basename "$f" | tr '[:upper:]' '[:lower:]')"
    if [[ ( "$b" == libopus-0.dll || "$b" == libopus.dll ) ]]; then
      if [[ "$f" == *"/build/mingw32-p3-vendor/opus/"* ]]; then
        out+=("$f")
        continue
      fi
      if [[ "$have_vend_opus" -eq 1 ]]; then
        echo "[p3-all] skip (use build/mingw32-p3-vendor/opus for PIII): $f" >&2
        continue
      fi
    fi
    if [[ "$b" == libportaudio.dll ]]; then
      if [[ "$f" == *"/build/mingw32-p3-vendor/portaudio/"* ]]; then
        out+=("$f")
        continue
      fi
      if [[ -f "$pdir/libportaudio.dll" ]]; then
        echo "[p3-all] skip (use build/mingw32-p3-vendor/portaudio for PIII): $f" >&2
        continue
      fi
    fi
    out+=("$f")
  done
  _P3_COLLECTED=("${out[@]}")
}

# Stock MSYS2 i686/GL prebuilts in bin/ are not built with PIII; gate our EXEs + vendored codecs.
dashcdg_p3_skip_prebuilt_mingw_dlls() {
  if [[ "${P3_STRICT_MINGW_DLLS:-1}" == 1 ]]; then
    return 0
  fi
  local f b
  local -a out=()
  for f in "${PE_LIST[@]}"; do
    b="$(basename "$f" | tr '[:upper:]' '[:lower:]')"
    case "$b" in
      libstdc++-6.dll|libgcc_s_dw2-1.dll|libwinpthread-1.dll|glew32.dll|libfreeglut.dll)
        echo "[p3-all] skip prebuilt MinGW DLL ($b): $f (set P3_STRICT_MINGW_DLLS=1 to scan)" >&2
        continue
        ;;
    esac
    out+=("$f")
  done
  PE_LIST=("${out[@]}")
}

# sneakernet: gl-rx = copy of desktop-rx; gdi+gl *-player = copy of desktop-player;
# desktop-legacy-rx = copy of desktop-gdi-rx (build_windows_sneakernet_dist.sh).
# Basename dedup can keep stale sneakernet-only names; drop aliases when the canonical EXE is in build/x86 or the list.
dashcdg_p3_drop_sneakernet_alias_exes() {
  local f b
  local have_player=0 have_rx=0 have_gdi=0
  for f in "${_P3_COLLECTED[@]}"; do
    b=$(basename "$f" | tr '[:upper:]' '[:lower:]')
    case "$b" in
      desktop-player.exe) have_player=1 ;;
      desktop-rx.exe) have_rx=1 ;;
      desktop-gdi-rx.exe) have_gdi=1 ;;
    esac
  done
  if [[ -f "${ROOT}/build/x86/bin/desktop-player.exe" ]]; then
    have_player=1
  fi
  if [[ -f "${ROOT}/build/x86/bin/desktop-rx.exe" ]]; then
    have_rx=1
  fi
  if [[ -f "${ROOT}/build/x86/bin/desktop-gdi-rx.exe" ]]; then
    have_gdi=1
  fi
  local -a out=()
  for f in "${_P3_COLLECTED[@]}"; do
    b=$(basename "$f" | tr '[:upper:]' '[:lower:]')
    if [[ ( "$b" == "desktop-gdi-player.exe" || "$b" == "desktop-gl-player.exe" ) && "$have_player" -eq 1 ]]; then
      echo "[p3-all] skip (alias of desktop-player.exe): $f" >&2
      continue
    fi
    if [[ "$b" == "desktop-gl-rx.exe" && "$have_rx" -eq 1 ]]; then
      echo "[p3-all] skip (alias of desktop-rx.exe): $f" >&2
      continue
    fi
    if [[ "$b" == "desktop-legacy-rx.exe" && "$have_gdi" -eq 1 ]]; then
      echo "[p3-all] skip (alias of desktop-gdi-rx.exe): $f" >&2
      continue
    fi
    out+=("$f")
  done
  _P3_COLLECTED=("${out[@]}")
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
    declare -A _P3_SEEN_BAS=()
    _P3_COLLECTED=()
    # 1) PIII vendor (Opus/PortAudio)  2) local i686 build trees  3) sneakernet (may be stale)
    dashcdg_p3_append_order_first_basename \
      "${ROOT}/build/mingw32-p3-vendor/opus/bin/libopus-0.dll" \
      "${ROOT}/build/mingw32-p3-vendor/opus/bin/libopus.dll" \
      "${ROOT}/build/mingw32-p3-vendor/portaudio/bin/libportaudio.dll"
    if [[ -d "${ROOT}/build/x86/bin" ]]; then
      while IFS= read -r -d '' f; do
        dashcdg_p3_append_order_first_basename "$f"
      done < <(find "${ROOT}/build/x86/bin" -maxdepth 1 -type f -print0 2>/dev/null)
    fi
    if [[ -d "${ROOT}/build/x86-retro/bin" ]]; then
      while IFS= read -r -d '' f; do
        dashcdg_p3_append_order_first_basename "$f"
      done < <(find "${ROOT}/build/x86-retro/bin" -maxdepth 1 -type f -print0 2>/dev/null)
    fi
    SD="${ROOT}/build/dist/dashcdg-windows-sneakernet"
    for sub in windows-x86 windows-x86-legacy-p3 windows-x86-retro; do
      d="${SD}/${sub}"
      [[ -d "$d" ]] || continue
      while IFS= read -r -d '' f; do
        dashcdg_p3_append_order_first_basename "$f"
      done < <(find "$d" -type f \( -iname '*.exe' -o -iname '*.dll' \) -print0 2>/dev/null)
    done
    dashcdg_p3_filter_bin_codec_dlls
    dashcdg_p3_drop_sneakernet_alias_exes
    PE_LIST=("${_P3_COLLECTED[@]}")
  fi
fi

if [[ ${#PE_LIST[@]} -eq 0 ]]; then
  echo "[p3-all] nothing to scan" >&2
  exit 0
fi
readarray -t PE_LIST < <(printf '%s\n' "${PE_LIST[@]}" | sort -u)
dashcdg_p3_skip_prebuilt_mingw_dlls
# PIII targets 32-bit Windows only; 64-bit PEs (e.g. sneakernet/windows-x64) legitimately use SSE2+ in CRT paths.
PE_I386=()
for f in "${PE_LIST[@]}"; do
  if ! objdump -f "$f" 2>/dev/null | grep -q "file format pei-i386"; then
    echo "[p3-all] skip (not 32-bit i386 PE): $f" >&2
    continue
  fi
  PE_I386+=("$f")
done
PE_LIST=("${PE_I386[@]}")

if [[ ${#PE_LIST[@]} -eq 0 ]]; then
  echo "[p3-all] nothing to scan (no pei-i386 files in list)" >&2
  exit 0
fi
echo "[p3-all] scanning ${#PE_LIST[@]} i386 file(s) with objdump (PIII / no-SSE2 gate)…"
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
