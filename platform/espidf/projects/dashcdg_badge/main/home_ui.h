#pragma once

#include "esp_err.h"
#include "lvgl.h"

/** Stop status-bar timer (call before leaving home so LVGL objects are not updated after destroy). */
void dashcdg_home_ui_pause_status_updates(void);

esp_err_t dashcdg_home_ui_present(lv_disp_t *disp);
