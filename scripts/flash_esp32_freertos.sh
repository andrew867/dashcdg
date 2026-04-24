#!/usr/bin/env bash
# Flash dashcdg_badge to serial port (esptool uses RTS/DTR for auto-reset + GPIO0 boot by default).
#
# Usage:
#   bash scripts/flash_esp32_freertos.sh           # ESPPORT env or COM6 default
#   bash scripts/flash_esp32_freertos.sh COM6
#   ESPPORT=COM7 bash scripts/flash_esp32_freertos.sh
#
# Optional:
#   DASHCDG_ESP_MONITOR=1  — run serial monitor after flash (Ctrl+] to exit)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

export IDF_PATH="${IDF_PATH:-${REPO_ROOT}/third_party/esp-idf}"
PROJECT_DIR="${DASHCDG_ESP_IDF_PROJECT:-${REPO_ROOT}/platform/espidf/projects/dashcdg_badge}"

if [[ -n "${1:-}" ]]; then
  export ESPPORT="$1"
elif [[ -z "${ESPPORT:-}" ]]; then
  export ESPPORT="COM6"
fi

if [[ ! -f "${IDF_PATH}/tools/idf.py" ]]; then
  echo "[esp32-flash] ESP-IDF not found at IDF_PATH=${IDF_PATH}" >&2
  exit 1
fi

# shellcheck source=/dev/null
source "${IDF_PATH}/export.sh"

echo "[esp32-flash] ESPPORT=${ESPPORT}"
echo "[esp32-flash] PROJECT_DIR=${PROJECT_DIR}"

if [[ ! -f "${PROJECT_DIR}/build/flasher_args.json" ]] && [[ ! -f "${PROJECT_DIR}/build/dashcdg_badge.bin" ]]; then
  echo "[esp32-flash] no build output; run: bash scripts/build_esp32_freertos_platform.sh" >&2
  exit 1
fi

idf.py -C "${PROJECT_DIR}" flash

if [[ "${DASHCDG_ESP_MONITOR:-0}" == "1" ]]; then
  idf.py -C "${PROJECT_DIR}" monitor
fi
