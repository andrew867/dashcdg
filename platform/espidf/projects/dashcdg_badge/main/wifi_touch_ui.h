#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * Touch-first Wi-Fi STA setup: scan, pick SSID, password + on-screen keyboard, NVS persist on connect.
 * No captive portal / web UI (flash budget + offline simplicity).
 */
esp_err_t dashcdg_wifi_touch_ui_start(lv_disp_t *disp);
