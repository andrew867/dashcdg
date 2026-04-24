#pragma once

#include "esp_err.h"

#include "lvgl.h"

/** Touch beep + PWM level, YM-ish demo (Applications → Audio lab). */
esp_err_t dashcdg_audio_lab_ui_present(lv_disp_t *disp);

/**
 * When true, the next `dashcdg_audio_lab_ui_present` shows back as "apps" and returns to Applications.
 * Cleared after consume; call `false` from `dashcdg_nav_settings` / `dashcdg_nav_home` so the setup path stays default.
 */
void dashcdg_audio_lab_ui_set_return_to_apps_menu(bool return_to_apps);
