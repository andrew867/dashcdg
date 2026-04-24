# ESP-IDF platform

Firmware sources live under this tree; **`projects/dashcdg_badge/`** is the active ESP-IDF + FreeRTOS app (bring-up shell today, future badge/receiver firmware).

**Build / flash:** see **[`docs/embedded/esp-idf-setup.md`](../../docs/embedded/esp-idf-setup.md)** — `scripts/bootstrap_esp_idf.sh`, `scripts/build_esp32_freertos_platform.sh`, `scripts/flash_esp32_freertos.sh`.

**`dashcdg_badge` (current):** `esp_lcd` ST7789 + `atanisoft/esp_lcd_touch_xpt2046` + `esp_lvgl_port` / LVGL 9; touch UI for Wi-Fi scan/SSID/password (NVS), auto-reconnect from saved credentials. GPIO map: **`main/board_cyd_freenove_32.h`**.

**Partitions / OTA:** Custom table **`projects/dashcdg_badge/partitions_ota_4mb.csv`** — **ota_0** + **ota_1** (~1600 KiB each), **otadata** for A/B boot selection. Implements the physical backup needed before any update path. First time you switch from an old single-factory layout, do a full **`idf.py erase-flash`** (or chip erase) then flash so **otadata** is valid.

Future update channels (same binary layout): **ESP HTTPS OTA**, **custom UDP/multicast** or **session request to desktop TX** (stream into `esp_ota_write` on the *inactive* slot), or **copy from SD card** into the inactive slot — then `esp_ota_set_boot_partition` + reboot. Optional hardening later: bootloader **rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) plus app calling **`esp_ota_mark_app_valid_cancel_rollback()`** after self-tests.

**Desktop prerequisite:** Implementations should mirror the stabilized **protocol v4** receiver and timing rules validated on **Windows desktop-rx** (session_info reconfigure, jitter priming, `playback_base_*` / clock ownership, codec hot-swap, cold join, pause/resume). See **[`AGENTS.md`](../../AGENTS.md)**, **[`docs/hardware/esp32-receiver-architecture.md`](../../docs/hardware/esp32-receiver-architecture.md)**, and **[`.cursor/plans/esp32_embedded_enterprise_plan_b3bda7b3.plan.md`](../../.cursor/plans/esp32_embedded_enterprise_plan_b3bda7b3.plan.md)**.

Planned module split for larger firmware:

- `transport/`
- `display/`
- `storage/`
- `input/`
- `power/`
- `ota/`
