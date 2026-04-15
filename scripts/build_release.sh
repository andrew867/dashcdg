#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

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
kill_windows_binary_if_running "desktop-rx.exe"
kill_windows_binary_if_running "desktop-player.exe"
kill_posix_binary_if_running "desktop-tx"
kill_posix_binary_if_running "desktop-rx"
kill_posix_binary_if_running "desktop-player"

cd "${REPO_ROOT}"

echo "[build-release] building clean package"
mingw32-make clean package

echo "[build-release] release package ready:"
echo "  ${REPO_ROOT}/build/release/dashcdg-windows-portable.zip"
