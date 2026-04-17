#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_ARCH="${1:-all}"
MAKE_CMD="$(command -v make || command -v mingw32-make || true)"

if [[ -z "${MAKE_CMD}" ]]; then
  echo "[build-release] no make-compatible command found" >&2
  exit 1
fi

run_make_for_arch() {
  local arch="$1"
  local toolchain_bin=""
  local legacy_args=()

  case "${arch}" in
    mingw64) toolchain_bin="/c/msys64/mingw64/bin" ;;
    mingw32) toolchain_bin="/c/msys64/mingw32/bin" ;;
    *)
      echo "[build-release] unknown toolchain arch: ${arch}" >&2
      exit 1
      ;;
  esac

  if [[ "${DASHCDG_WINDOWS_LEGACY:-}" == "1" ]]; then
    legacy_args+=(WINDOWS_LEGACY_TARGET=1)
    echo "[build-release] WINDOWS_LEGACY_TARGET=1 (XP-oriented PE + WINVER)"
  fi

  PATH="${toolchain_bin}:$(dirname "${MAKE_CMD}"):${PATH}" \
    "${MAKE_CMD}" "${legacy_args[@]}" clean package "MINGW_ARCH=${arch}"
}

copy_zip_to_dist() {
  local zip_path="$1"

  mkdir -p "${REPO_ROOT}/build/dist"
  if [[ -f "${zip_path}" ]]; then
    cp -f "${zip_path}" "${REPO_ROOT}/build/dist/"
    echo "[build-release] dist: $(basename "${zip_path}") -> build/dist/"
  else
    echo "[build-release] warning: missing zip (skip dist copy): ${zip_path}" >&2
  fi
}

kill_windows_binary_if_running() {
  local image_name="$1"

  if command -v taskkill >/dev/null 2>&1; then
    taskkill //IM "${image_name}" //F >/dev/null 2>&1 || true
  fi
}

kill_posix_binary_if_running() {
  local process_name="$1"

  if command -v pkill >/dev/null 2>&1; then
    pkill -f "${process_name}" >/dev/null 2>&1 || true
  fi
}

echo "[build-release] stopping running desktop binaries"
kill_windows_binary_if_running "desktop-tx.exe"
kill_windows_binary_if_running "desktop-gdi-tx.exe"
kill_windows_binary_if_running "desktop-rx.exe"
kill_windows_binary_if_running "desktop-gdi-rx.exe"
kill_windows_binary_if_running "desktop-player.exe"
kill_posix_binary_if_running "desktop-tx"
kill_posix_binary_if_running "desktop-rx"
kill_posix_binary_if_running "desktop-player"

cd "${REPO_ROOT}"

build_mingw32_p3_codecs() {
  if [[ "${SKIP_MINGW32_P3_VENDOR:-}" == "1" ]]; then
    echo "[build-release] SKIP_MINGW32_P3_VENDOR=1 — reuse build/mingw32-p3-vendor (if present)"
    return 0
  fi
  echo "[build-release] building PIII-safe shared libopus-0.dll + libportaudio.dll"
  bash "${REPO_ROOT}/scripts/build_mingw32_p3_opus_portaudio_shared.sh"
}

case "${TARGET_ARCH}" in
  x64)
    echo "[build-release] building x64 package"
    run_make_for_arch mingw64
    copy_zip_to_dist "${REPO_ROOT}/build/amd64/release/dashcdg-windows-x64-portable.zip"
    echo "[build-release] release package ready:"
    echo "  ${REPO_ROOT}/build/amd64/release/dashcdg-windows-x64-portable.zip"
    ;;
  x86)
    echo "[build-release] building x86 package"
    build_mingw32_p3_codecs
    run_make_for_arch mingw32
    copy_zip_to_dist "${REPO_ROOT}/build/x86/release/dashcdg-windows-x86-portable.zip"
    echo "[build-release] release package ready:"
    echo "  ${REPO_ROOT}/build/x86/release/dashcdg-windows-x86-portable.zip"
    ;;
  all)
    echo "[build-release] building x64 and x86 packages"
    run_make_for_arch mingw64
    build_mingw32_p3_codecs
    run_make_for_arch mingw32
    copy_zip_to_dist "${REPO_ROOT}/build/amd64/release/dashcdg-windows-x64-portable.zip"
    copy_zip_to_dist "${REPO_ROOT}/build/x86/release/dashcdg-windows-x86-portable.zip"
    echo "[build-release] release packages ready:"
    echo "  ${REPO_ROOT}/build/amd64/release/dashcdg-windows-x64-portable.zip"
    echo "  ${REPO_ROOT}/build/x86/release/dashcdg-windows-x86-portable.zip"
    echo "[build-release] unified dist copies:"
    echo "  ${REPO_ROOT}/build/dist/dashcdg-windows-x64-portable.zip"
    echo "  ${REPO_ROOT}/build/dist/dashcdg-windows-x86-portable.zip"
    ;;
  *)
    echo "usage: ${0} [x64|x86|all]" >&2
    echo "  optional: DASHCDG_WINDOWS_LEGACY=1  (passes WINDOWS_LEGACY_TARGET=1 to make)" >&2
    exit 1
    ;;
esac
