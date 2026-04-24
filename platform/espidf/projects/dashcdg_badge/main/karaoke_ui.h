#pragma once

#include "esp_err.h"
#include "lvgl.h"

esp_err_t dashcdg_karaoke_ui_present(lv_disp_t *disp);

/** Stop karaoke timers / modal before replacing the screen (nav_home calls this). */
void dashcdg_karaoke_ui_teardown(void);
