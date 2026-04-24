#pragma once

#include "esp_err.h"
#include "lvgl.h"

/** Backlight, rear RGB, auto idle - opened from Settings. */
esp_err_t dashcdg_display_ui_present(lv_disp_t *disp);
