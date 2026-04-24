#!/usr/bin/env bash
# Clone Espressif ESP-IDF (includes FreeRTOS under components/freertos) into third_party/esp-idf
# and install toolchains + Python env. Run once per machine (or after deleting third_party/esp-idf).
#
# Official docs: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/
# GitHub:       https://github.com/espressif/esp-idf
#
# Requires: git, cmake, ninja (optional but recommended), Python 3.9+
# Windows: Git Bash can run install.bat via cmd; or use Espressif's Windows Installer and set IDF_PATH.
#
# Env:
#   DASHCDG_ESP_IDF_REF=v5.5.4   — branch or tag (default below; match GUI installer if possible)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DASHCDG_ESP_IDF_REF="${DASHCDG_ESP_IDF_REF:-v5.5.4}"
IDF_DEST="${REPO_ROOT}/third_party/esp-idf"

echo "[bootstrap-esp-idf] repo root: ${REPO_ROOT}"
echo "[bootstrap-esp-idf] IDF ref:   ${DASHCDG_ESP_IDF_REF}"
echo "[bootstrap-esp-idf] dest:      ${IDF_DEST}"

mkdir -p "${REPO_ROOT}/third_party"

if [[ -d "${IDF_DEST}/.git" ]]; then
  echo "[bootstrap-esp-idf] existing clone at ${IDF_DEST}; fetching ${DASHCDG_ESP_IDF_REF} ..."
  git -C "${IDF_DEST}" fetch --tags origin 2>/dev/null || true
  git -C "${IDF_DEST}" checkout "${DASHCDG_ESP_IDF_REF}"
  git -C "${IDF_DEST}" submodule update --init --recursive
else
  echo "[bootstrap-esp-idf] cloning esp-idf (recursive submodules; includes FreeRTOS) ..."
  git clone --branch "${DASHCDG_ESP_IDF_REF}" --recursive \
    https://github.com/espressif/esp-idf.git "${IDF_DEST}"
fi

cd "${IDF_DEST}"

run_install_win_bat() {
  if [[ -f "install.bat" ]]; then
    echo "[bootstrap-esp-idf] running install.bat esp32 (Windows toolchain download) ..."
    local widf
    if command -v cygpath >/dev/null 2>&1; then
      widf="$(cygpath -w "$(pwd)")"
      cmd.exe //c "cd /d \"${widf}\" && install.bat esp32"
    else
      cmd.exe //c "install.bat esp32"
    fi
    return 0
  fi
  return 1
}

run_install_sh() {
  if [[ -f "install.sh" ]]; then
    echo "[bootstrap-esp-idf] running ./install.sh esp32 ..."
    chmod +x install.sh export.sh 2>/dev/null || true
    ./install.sh esp32
    return 0
  fi
  return 1
}

case "$(uname -s 2>/dev/null || printf '')" in
  MINGW*|MSYS*|CYGWIN*)
    run_install_win_bat || run_install_sh || {
      echo "[bootstrap-esp-idf] install failed; open ESP-IDF PowerShell or cmd and run:" >&2
      echo "  cd /d \"${IDF_DEST}\"" >&2
      echo "  install.bat esp32" >&2
      exit 1
    }
    ;;
  *)
    run_install_sh || {
      echo "[bootstrap-esp-idf] ./install.sh missing or failed." >&2
      exit 1
    }
    ;;
esac

echo "[bootstrap-esp-idf] done."
echo ""
echo "Next (each new shell):"
echo "  source \"${IDF_DEST}/export.sh\""
echo "Or build:"
echo "  bash scripts/build_esp32_freertos_platform.sh"
