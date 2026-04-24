#pragma once

#include "esp_err.h"
#include "lvgl.h"

/** ST7789 + XPT2046 + esp_lvgl_port (FreeRTOS task). Tune mirrors in display_lvgl.c if colors/axes swap. */
esp_err_t dashcdg_display_lvgl_init(lv_disp_t **out_disp);
