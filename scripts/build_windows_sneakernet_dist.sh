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

cd "${REPO_ROOT}"

run_make_debug() {
  local arch="$1"
  shift
  local extra_make_args=("$@")
  local toolchain_bin=""

  case "${arch}" in
    mingw64) toolchain_bin="/c/msys64/mingw64/bin" ;;
    mingw32) toolchain_bin="/c/msys64/mingw32/bin" ;;
    *)
      echo "[sneakernet-dist] unknown toolchain arch: ${arch}" >&2
      exit 1
      ;;
  esac

  PATH="${toolchain_bin}:$(dirname "${MAKE_CMD}"):${PATH}" \
    "${MAKE_CMD}" clean debug MINGW_ARCH="${arch}" "${extra_make_args[@]}"
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

windows-x86/
  32-bit MSYS2 mingw32 (typical i686/SSE2-era runtime DLLs from the prefix).

windows-x86-legacy-p3/
  Same as windows-x86 but dashcdg objects built with -march=pentium3 and XP-oriented
  PE flags (see docs/specs/windows-legacy-mingw-build.md). Third-party DLLs may still
  need a custom libopus build for real P3 machines.

windows-x86-retro/
  “Retro” bundle: Win2000-style PE targets, -march=pentium2, no Opus, no OpenGL.
  desktop-retro-rx.exe = GDI receiver; desktop-retro-tx.exe = transmitter (no --display).
  Use --badnet-v4 and SBC-like audio with matching sender/receiver.

Executables (standard folders)
------------------------------
  desktop-tx.exe         Headless transmitter (Opus or SBC-like via --badnet-v4 / profiles)
  desktop-gdi-tx.exe     Transmitter + Win32 GDI preview (no GL); --headless to hide window
  desktop-rx.exe         Receiver: OpenGL by default; --gdi forces GDI; Windows auto-fallback if GL fails
  desktop-gl-rx.exe      Same bits as desktop-rx.exe (alias)
  desktop-gdi-rx.exe     GDI-only receiver link (no GL DLL dependency for this EXE)
  desktop-player.exe     Local player + `tx` / `rx` subcommands (full GL + GDI code paths)
  desktop-*-player.exe   Copies of desktop-player.exe (legacy filenames)

EOF
}

kill_running

rm -rf "${DIST_ROOT}"
mkdir -p "${DIST_ROOT}"

echo "[sneakernet-dist] (1/4) windows-x64 — mingw64"
run_make_debug mingw64
layout_standard_variant "${REPO_ROOT}/build/amd64/bin" "${DIST_ROOT}/windows-x64"

echo "[sneakernet-dist] (2/4) windows-x86 — mingw32"
run_make_debug mingw32
layout_standard_variant "${REPO_ROOT}/build/x86/bin" "${DIST_ROOT}/windows-x86"

echo "[sneakernet-dist] (3/4) windows-x86-legacy-p3 — mingw32 WINDOWS_LEGACY_TARGET=1"
run_make_debug mingw32 WINDOWS_LEGACY_TARGET=1
layout_standard_variant "${REPO_ROOT}/build/x86/bin" "${DIST_ROOT}/windows-x86-legacy-p3"

echo "[sneakernet-dist] (4/4) windows-x86-retro — mingw32 WINDOWS_RETRO_BUNDLE=1"
run_make_debug mingw32 WINDOWS_RETRO_BUNDLE=1
layout_retro_variant "${REPO_ROOT}/build/x86-retro/bin" "${DIST_ROOT}/windows-x86-retro"

write_readme

rm -f "${OUT_ZIP}"

if command -v powershell.exe >/dev/null 2>&1; then
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
