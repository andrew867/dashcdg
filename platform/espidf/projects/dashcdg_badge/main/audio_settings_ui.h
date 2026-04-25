#pragma once

#include "esp_err.h"

#include "lvgl.h"

/** Setup hub: speaker PWM level + touch chirp on/off (NVS). Mary demo is Applications -> Audio lab only. */
esp_err_t dashcdg_audio_settings_ui_present(lv_disp_t *disp);
