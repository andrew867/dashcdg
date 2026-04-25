#pragma once

#include "esp_err.h"

#include "lvgl.h"

/** Mary PWM demo + status strip (Applications only). Speaker/touch NVS: Setup -> Audio & touch beeps. */
esp_err_t dashcdg_audio_lab_ui_present(lv_disp_t *disp);
