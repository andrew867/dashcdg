#!/usr/bin/env bash
# Build dashcdg_badge firmware (ESP-IDF + FreeRTOS).
#
# One-time: bash scripts/bootstrap_esp_idf.sh
#
# Env:
#   IDF_PATH              — override ESP-IDF tree (default: REPO/third_party/esp-idf)
#   DASHCDG_ESP_IDF_PROJECT — CMake project dir (default: platform/espidf/projects/dashcdg_badge)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

export IDF_PATH="${IDF_PATH:-${REPO_ROOT}/third_party/esp-idf}"
PROJECT_DIR="${DASHCDG_ESP_IDF_PROJECT:-${REPO_ROOT}/platform/espidf/projects/dashcdg_badge}"

if [[ ! -f "${IDF_PATH}/tools/idf.py" ]]; then
  echo "[esp32-build] ESP-IDF not found at IDF_PATH=${IDF_PATH}" >&2
  echo "[esp32-build] Run: bash scripts/bootstrap_esp_idf.sh" >&2
  exit 1
fi

# shellcheck source=/dev/null
source "${IDF_PATH}/export.sh"

if [[ ! -d "${PROJECT_DIR}" ]]; then
  echo "[esp32-build] project dir missing: ${PROJECT_DIR}" >&2
  exit 1
fi

cd "${REPO_ROOT}"

if [[ ! -f "${PROJECT_DIR}/sdkconfig" ]] && [[ ! -f "${PROJECT_DIR}/sdkconfig.defaults" ]]; then
  echo "[esp32-build] warning: no sdkconfig.defaults in ${PROJECT_DIR}" >&2
fi

if [[ ! -f "${PROJECT_DIR}/sdkconfig" ]]; then
  echo "[esp32-build] first-time configure: set-target esp32"
  idf.py -C "${PROJECT_DIR}" set-target esp32
fi

echo "[esp32-build] IDF_PATH=${IDF_PATH}"
echo "[esp32-build] PROJECT_DIR=${PROJECT_DIR}"
echo "[esp32-build] tip: CMAKE_BUILD_PARALLEL_LEVEL=N limits ninja parallelism"

idf.py -C "${PROJECT_DIR}" build

BIN="${PROJECT_DIR}/build/dashcdg_badge.bin"
echo "[esp32-build] ok → ${BIN}"
echo "[esp32-flash] bash scripts/flash_esp32_freertos.sh COM6"
