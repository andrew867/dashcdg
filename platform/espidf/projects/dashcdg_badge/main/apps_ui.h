#pragma once

#include "esp_err.h"

#include "lvgl.h"

/** Launcher → Applications: built-in apps (Audio lab first); back returns home. */
esp_err_t dashcdg_applications_ui_present(lv_disp_t *disp);
