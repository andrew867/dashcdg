#!/usr/bin/env bash
# One sneakernet-ready tree under build/dist/dashcdg-windows-sneakernet/ plus a zip of that folder.
#
# Builds (each in its own MSYS2 prefix + build/ tree):
#   1. windows-x64          — mingw64, default PE, GL+GDI RX + TX + player
#   2. windows-x86          — mingw32 i686, default PE, GL+GDI RX + TX + player
#   3. windows-x86-legacy-p3 — mingw32 + WINDOWS_LEGACY_TARGET=1 (XP PE + -march=pentium3)
#   4. windows-x86-retro    — mingw32 + WINDOWS_RETRO_BUNDLE=1 (Win2000 PE, pentium3,
#                             GDI+SBC-only RX/TX; no Opus / no GL stack)
#
# Standard folders ship distinct EXEs: headless desktop-tx, GL desktop-rx, GDI RX/TX,
# full desktop-player. Retro folder uses desktop-retro-*.exe + minimal DLLs.
#
# Speed (default — incremental, parallel):
#   - Does NOT run `make clean` unless DASHCDG_SNEAKENET_CLEAN=1 (reuse object files).
#   - Runs `make -jN` with N = DASHCDG_SNEAKENET_JOBS or CPU count (parallel compilation).
#   - Builds amd64 and x86 in parallel (separate BUILD_DIR), then legacy + retro in parallel.
#
# Optional env:
#   DASHCDG_SNEAKENET_CLEAN=1     — full rebuild (make clean debug) per variant (slow, CI-like).
#   DASHCDG_SNEAKENET_JOBS=N      — make parallelism (default: nproc or 8).
#   DASHCDG_SNEAKENET_ZIP_FAST=1  — use `zip -1` if available (faster, slightly larger .zip).
#   DASHCDG_KILL_RUNNING_DESKTOP_BINS=1 — taskkill desktop-*.exe before packaging
#   SKIP_MINGW32_P3_VENDOR=1      — skip rebuilding PIII vendor codecs
#   SKIP_SOXR_VENDOR=1             — skip static libsoxr mingw64/mingw32 builds (make will fail if libs missing)
#   RUN_P3_DISASM=1               — run full objdump PIII gate on dist + build trees (slow; CI / release)
#   SKIP_P3_DISASM=1              — force-skip objdump gate (overrides RUN_P3_DISASM; default is already skip)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MAKE_CMD="$(command -v make || command -v mingw32-make || true)"

if [[ -z "${MAKE_CMD}" ]]; then
  echo "[sneakernet-dist] no make command found" >&2
  exit 1
fi

DIST_ROOT="${REPO_ROOT}/build/dist/dashcdg-windows-sneakernet"
OUT_ZIP="${REPO_ROOT}/build/dist/dashcdg-windows-sneakernet.zip"

# Parallel make jobs: explicit > nproc > 8
if [[ -n "${DASHCDG_SNEAKENET_JOBS:-}" ]]; then
  SNEAKERNET_MAKE_JOBS="${DASHCDG_SNEAKENET_JOBS}"
elif command -v nproc >/dev/null 2>&1; then
  SNEAKERNET_MAKE_JOBS="$(nproc)"
else
  SNEAKERNET_MAKE_JOBS="8"
fi

cd "${REPO_ROOT}"

# MSYS2 root: prefer canonical installs before inferring from PATH. A standalone MinGW-w64
# tree (e.g. under /c/ProgramData/mingw64) often appears first as x86_64-w64-mingw32-gcc but
# does not ship MSYS2-style dev packages (GL/glew.h, etc.), which breaks mingw64 GL builds.
dashcdg_resolve_msys2_root() {
  local r gcc64 bin64
  r="${MSYS2_ROOT:-}"
  if [[ -n "$r" ]] && [[ -d "$r/mingw64/bin" ]] && [[ -d "$r/mingw32/bin" ]]; then
    echo "${r%/}"
    return 0
  fi
  for r in /c/msys64 /c/msys2; do
    if [[ -d "$r/mingw64/bin" && -d "$r/mingw32/bin" ]]; then
      echo "$r"
      return 0
    fi
  done
  gcc64="$(command -v x86_64-w64-mingw32-gcc 2>/dev/null || true)"
  if [[ -n "$gcc64" ]]; then
    bin64="$(cd "$(dirname "$gcc64")" && pwd)"
    (cd "$bin64/../.." && pwd)
    return 0
  fi
  echo ""
  return 1
}

DASHCDG_MSYS2_ROOT="$(dashcdg_resolve_msys2_root || true)"
if [[ -z "$DASHCDG_MSYS2_ROOT" ]]; then
  echo "[sneakernet-dist] could not find MSYS2 root (install mingw64+mingw32 toolchains or set MSYS2_ROOT)" >&2
  exit 1
fi
export MSYS2_ROOT="$DASHCDG_MSYS2_ROOT"

run_make_variant() {
  local label="$1"
  local arch="$2"
  shift 2
  local extra_make_args=("$@")
  local toolchain_bin=""

  case "${arch}" in
    mingw64) toolchain_bin="${DASHCDG_MSYS2_ROOT}/mingw64/bin" ;;
    mingw32) toolchain_bin="${DASHCDG_MSYS2_ROOT}/mingw32/bin" ;;
    *)
      echo "[sneakernet-dist] unknown toolchain arch: ${arch}" >&2
      return 1
      ;;
  esac

  echo "[sneakernet-dist] building ${label} (${arch})…"
  (
    cd "${REPO_ROOT}"
    export PATH="${toolchain_bin}:$(dirname "${MAKE_CMD}"):${PATH}"
    # Never pass `clean` and `debug` in one -j make: targets could run in parallel.
    if [[ "${DASHCDG_SNEAKENET_CLEAN:-0}" == "1" ]]; then
      "${MAKE_CMD}" clean MINGW_ARCH="${arch}" "${extra_make_args[@]}"
    fi
    "${MAKE_CMD}" -j"${SNEAKERNET_MAKE_JOBS}" debug MINGW_ARCH="${arch}" "${extra_make_args[@]}"
  )
}

# Force PIII-built Opus + PortAudio next to exes (never rely on stale or MSYS2 copies from a prior build/ dir).
copy_p3_codec_dlls() {
  local destdir="${1?}"
  local p3o="${REPO_ROOT}/build/mingw32-p3-vendor/opus/bin"
  local p3p="${REPO_ROOT}/build/mingw32-p3-vendor/portaudio/bin"

  if [[ ! -f "${p3o}/libopus-0.dll" && ! -f "${p3o}/libopus.dll" ]]; then
    echo "[sneakernet] missing P3 libopus in ${p3o} (build_mingw32_p3_opus_portaudio_shared.sh did not install a DLL?)" >&2
    return 1
  fi
  if [[ ! -f "${p3p}/libportaudio.dll" ]]; then
    echo "[sneakernet] missing ${p3p}/libportaudio.dll" >&2
    return 1
  fi

  # Always ship the MSYS2/loader name next to EXEs (`-lopus` → libopus-0.dll).
  if [[ -f "${p3o}/libopus-0.dll" ]]; then
    cp -f "${p3o}/libopus-0.dll" "${destdir}/"
  elif [[ -f "${p3o}/libopus.dll" ]]; then
    cp -f "${p3o}/libopus.dll" "${destdir}/libopus-0.dll"
  fi
  rm -f "${destdir}/libopus.dll"
  cp -f "${p3p}/libportaudio.dll" "${destdir}/"
}

# Standard layout: real binaries + optional GL-RX alias name for muscle memory.
layout_standard_variant() {
  local src_bin_dir="$1"
  local dest_dir="$2"

  if [[ ! -d "${src_bin_dir}" ]]; then
    echo "[sneakernet-dist] missing bin dir: ${src_bin_dir}" >&2
    exit 1
  fi

  mkdir -p "${dest_dir}"
  cp -f "${src_bin_dir}/desktop-tx.exe" "${dest_dir}/"
  cp -f "${src_bin_dir}/desktop-gdi-tx.exe" "${dest_dir}/"
  cp -f "${src_bin_dir}/desktop-rx.exe" "${dest_dir}/"
  cp -f "${src_bin_dir}/desktop-rx.exe" "${dest_dir}/desktop-gl-rx.exe"
  cp -f "${src_bin_dir}/desktop-gdi-rx.exe" "${dest_dir}/"
  cp -f "${src_bin_dir}/desktop-player.exe" "${dest_dir}/"
  cp -f "${src_bin_dir}/desktop-player.exe" "${dest_dir}/desktop-gl-player.exe"
  cp -f "${src_bin_dir}/desktop-player.exe" "${dest_dir}/desktop-gdi-player.exe"
  shopt -s nullglob
  for f in "${src_bin_dir}"/*.dll; do
    cp -f "${f}" "${dest_dir}/"
  done
  shopt -u nullglob
}

layout_retro_variant() {
  local src_bin_dir="$1"
  local dest_dir="$2"

  if [[ ! -d "${src_bin_dir}" ]]; then
    echo "[sneakernet-dist] missing retro bin dir: ${src_bin_dir}" >&2
    exit 1
  fi

  mkdir -p "${dest_dir}"
  cp -f "${src_bin_dir}/desktop-retro-rx.exe" "${dest_dir}/"
  cp -f "${src_bin_dir}/desktop-retro-tx.exe" "${dest_dir}/"
  shopt -s nullglob
  for f in "${src_bin_dir}"/*.dll; do
    cp -f "${f}" "${dest_dir}/"
  done
  shopt -u nullglob
}

kill_running() {
  if command -v taskkill >/dev/null 2>&1; then
    for im in desktop-tx.exe desktop-gdi-tx.exe desktop-rx.exe desktop-gdi-rx.exe desktop-player.exe desktop-retro-tx.exe desktop-retro-rx.exe; do
      taskkill //IM "${im}" //F >/dev/null 2>&1 || true
    done
  fi
}

write_readme() {
  cat > "${DIST_ROOT}/README.txt" <<'EOF'
dashcdg — Windows sneakernet bundle
====================================

Copy this entire folder (or use the sibling .zip). Each subfolder is self-contained
(EXE + DLLs next to them).

Subfolders
----------
windows-x64/
  64-bit MSYS2 mingw64. Headless desktop-tx + desktop-gdi-tx (Win32 preview, no GL);
  desktop-rx (GL default, GDI fallback on GL init failure) + desktop-gdi-rx + desktop-player.
  Audio SRC uses static libsoxr (LGPL) linked into EXEs — no libsoxr DLL.

windows-x86/
  32-bit mingw32. libopus-0.dll and libportaudio.dll come from build/mingw32-p3-vendor/
  (PIII / no-SSE2–safe; see scripts/build_mingw32_p3_opus_portaudio_shared.sh).
  Static libsoxr for SRC is built Pentium III–safe (no SSE SIMD engines); linked into EXEs.

windows-x86-legacy-p3/
  Same codecs as windows-x86; dashcdg objects use -march=pentium3 + XP PE (WINDOWS_LEGACY_TARGET=1).
  Includes desktop-legacy-rx.exe (copy of desktop-gdi-rx.exe) for muscle memory.

windows-x86-retro/
  Win2000-style PE, -march=pentium3, no OpenGL. desktop-retro-rx.exe / desktop-retro-tx.exe use real
  Opus decode/encode + PortAudio with the same PIII-safe libopus-0.dll and libportaudio.dll as other
  mingw32 folders (not WinMM-only). Default TX audio is Opus; use TTY `c` or flags to change codec.

Executables (standard folders)
------------------------------
  desktop-tx.exe         Headless transmitter (Opus or SBC-like via --badnet-v4 / profiles)
  desktop-gdi-tx.exe     Transmitter + Win32 GDI preview (no GL); --headless to hide window
  desktop-rx.exe         Receiver: OpenGL by default; --gdi forces GDI; Windows auto-fallback if GL fails
  desktop-gl-rx.exe      Same bits as desktop-rx.exe (alias)
  desktop-gdi-rx.exe     GDI-only receiver link (no GL DLL dependency for this EXE)
  desktop-legacy-rx.exe  Same bits as desktop-gdi-rx.exe (alias; x86 + legacy-p3 folders)
  desktop-player.exe     Local player + `tx` / `rx` subcommands (full GL + GDI code paths)
  desktop-*-player.exe   Copies of desktop-player.exe (legacy filenames)

EOF
}

if [[ "${DASHCDG_KILL_RUNNING_DESKTOP_BINS:-}" == "1" ]]; then
  echo "[sneakernet-dist] DASHCDG_KILL_RUNNING_DESKTOP_BINS=1 — stopping running desktop binaries"
  kill_running
fi

rm -rf "${DIST_ROOT}"
mkdir -p "${DIST_ROOT}"

if [[ "${SKIP_MINGW32_P3_VENDOR:-}" == "1" ]]; then
  echo "[sneakernet-dist] SKIP_MINGW32_P3_VENDOR=1 — not rebuilding build/mingw32-p3-vendor DLLs"
else
  echo "[sneakernet-dist] building PIII-safe libopus-0.dll + libportaudio.dll (mingw32)"
  bash "${SCRIPT_DIR}/build_mingw32_p3_opus_portaudio_shared.sh"
fi

if [[ "${SKIP_SOXR_VENDOR:-}" == "1" ]]; then
  echo "[sneakernet-dist] SKIP_SOXR_VENDOR=1 — not rebuilding static libsoxr (ensure build/soxr-vendor-mingw*/install/lib/libsoxr.a exists)"
else
  echo "[sneakernet-dist] building static libsoxr (mingw64 + mingw32 PIII-safe)"
  bash "${SCRIPT_DIR}/build_soxr_vendor.sh" mingw64
  bash "${SCRIPT_DIR}/build_soxr_vendor.sh" mingw32
fi

if [[ "${DASHCDG_SNEAKENET_CLEAN:-0}" == "1" ]]; then
  echo "[sneakernet-dist] DASHCDG_SNEAKENET_CLEAN=1 — full rebuild (clean + debug) per variant"
else
  echo "[sneakernet-dist] incremental: no make clean (set DASHCDG_SNEAKENET_CLEAN=1 for full rebuild)"
fi
echo "[sneakernet-dist] make -j${SNEAKERNET_MAKE_JOBS} per variant"

# Phase 1: amd64 and x86 use disjoint BUILD_DIR — build in parallel.
run_make_variant "windows-x64" mingw64 &
pid_amd64=$!
run_make_variant "windows-x86" mingw32 &
pid_x86=$!
wait "${pid_amd64}"
wait "${pid_x86}"

layout_standard_variant "${REPO_ROOT}/build/amd64/bin" "${DIST_ROOT}/windows-x64"
layout_standard_variant "${REPO_ROOT}/build/x86/bin" "${DIST_ROOT}/windows-x86"
copy_p3_codec_dlls "${DIST_ROOT}/windows-x86"
cp -f "${DIST_ROOT}/windows-x86/desktop-gdi-rx.exe" "${DIST_ROOT}/windows-x86/desktop-legacy-rx.exe"

# Phase 2: legacy (build/x86) and retro (build/x86-retro) are disjoint — parallel.
run_make_variant "windows-x86-legacy-p3" mingw32 WINDOWS_LEGACY_TARGET=1 &
pid_leg=$!
run_make_variant "windows-x86-retro" mingw32 WINDOWS_RETRO_BUNDLE=1 &
pid_ret=$!
wait "${pid_leg}"
wait "${pid_ret}"

layout_standard_variant "${REPO_ROOT}/build/x86/bin" "${DIST_ROOT}/windows-x86-legacy-p3"
copy_p3_codec_dlls "${DIST_ROOT}/windows-x86-legacy-p3"
cp -f "${DIST_ROOT}/windows-x86-legacy-p3/desktop-gdi-rx.exe" "${DIST_ROOT}/windows-x86-legacy-p3/desktop-legacy-rx.exe"

layout_retro_variant "${REPO_ROOT}/build/x86-retro/bin" "${DIST_ROOT}/windows-x86-retro"
copy_p3_codec_dlls "${DIST_ROOT}/windows-x86-retro"

write_readme

if [[ "${SKIP_P3_DISASM:-0}" == "1" ]]; then
  echo "[sneakernet-dist] SKIP_P3_DISASM=1 — skipping objdump PIII gate" >&2
elif [[ "${RUN_P3_DISASM:-0}" != "1" ]]; then
  echo "[sneakernet-dist] skipping objdump PIII gate (set RUN_P3_DISASM=1 to run verify_p3_pe_pentium3.sh)" >&2
elif command -v objdump >/dev/null 2>&1; then
  echo "[sneakernet-dist] RUN_P3_DISASM=1: objdump full PIII gate on every .exe/.dll in dist + PIII vendor + build/x86*/bin…"
  export P3_STRICT_MINGW_DLLS="${P3_STRICT_MINGW_DLLS:-0}"
  bash "${SCRIPT_DIR}/verify_p3_pe_pentium3.sh" "${DIST_ROOT}" || exit 1
else
  echo "[sneakernet-dist] FATAL: RUN_P3_DISASM=1 but objdump not on PATH (mingw-w64-i686-binutils)." >&2
  exit 1
fi

rm -f "${OUT_ZIP}"

if [[ "${DASHCDG_SNEAKENET_ZIP_FAST:-0}" == "1" ]] && command -v zip >/dev/null 2>&1; then
  echo "[sneakernet-dist] DASHCDG_SNEAKENET_ZIP_FAST=1 — zip -1 (fast compression)"
  (cd "${REPO_ROOT}/build/dist" && zip -r -q -1 "$(basename "${OUT_ZIP}")" "$(basename "${DIST_ROOT}")")
elif command -v powershell.exe >/dev/null 2>&1; then
  REPO_WIN="${REPO_ROOT}"
  if command -v cygpath >/dev/null 2>&1; then
    REPO_WIN="$(cygpath -w "${REPO_ROOT}")"
  fi
  powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \
    "Compress-Archive -Path '${REPO_WIN}/build/dist/dashcdg-windows-sneakernet' -DestinationPath '${REPO_WIN}/build/dist/dashcdg-windows-sneakernet.zip' -Force"
else
  echo "[sneakernet-dist] powershell.exe not found; copy the folder without zip" >&2
fi

echo "[sneakernet-dist] done."
echo "  Folder: ${DIST_ROOT}/"
echo "  Zip:    ${OUT_ZIP}"
