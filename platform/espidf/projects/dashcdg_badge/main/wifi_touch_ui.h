#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * Touch-first Wi-Fi STA setup: scan, pick SSID, password + on-screen keyboard.
 *
 * NVS namespace "dashcfg": ssid + psk written only after a successful association (STA got IP).
 * Browsing the SSID dropdown does not read or write NVS; only Connect applies a new candidate
 * to the Wi-Fi driver, and NVS updates only when that connection succeeds.
 *
 * dashcdg_wifi_ensure_init() starts the Wi-Fi driver once (safe to call from app_main).
 * dashcdg_wifi_boot_auto_connect() loads saved creds from NVS (if any) and starts connect — call
 * from app_main so the device auto-rejoins without opening the Wi-Fi screen.
 * dashcdg_wifi_touch_ui_present() clears the active LVGL screen and builds this UI.
 */
esp_err_t dashcdg_wifi_ensure_init(void);
/** Init Wi-Fi once and connect using NVS credentials when present; else ESP_ERR_NOT_FOUND. */
esp_err_t dashcdg_wifi_boot_auto_connect(void);
/** Invalidate stale LVGL pointers after the Wi-Fi screen is destroyed (e.g. when navigating away). */
void dashcdg_wifi_drop_lvgl_refs(void);

esp_err_t dashcdg_wifi_touch_ui_present(lv_disp_t *disp);

/** One-shot helper: ensure_init + present (e.g. tools that skip the launcher). */
esp_err_t dashcdg_wifi_touch_ui_start(lv_disp_t *disp);
