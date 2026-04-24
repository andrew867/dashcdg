#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

typedef void (*dashcdg_touch_cal_nav_fn)(lv_disp_t *disp);

/**
 * Four-corner raw ADC calibration (Freenove Sketch_11.1 style). Disables LVGL touch input while active.
 * @param show_cancel_button if false (first-boot wizard), user must finish; if true, returns via on_cancel.
 */
esp_err_t dashcdg_touch_cal_ui_present(lv_disp_t *disp, bool show_cancel_button, dashcdg_touch_cal_nav_fn on_done,
                                       dashcdg_touch_cal_nav_fn on_cancel);
