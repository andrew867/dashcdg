#pragma once

#include "lvgl.h"

/**
 * Switch active LVGL screen between launcher, Wi-Fi setup, and Karaoke placeholder.
 * Each target clears the active screen and rebuilds (simple app shell until SD-hosted apps exist).
 */
void dashcdg_nav_home(lv_disp_t *disp);
void dashcdg_nav_applications(lv_disp_t *disp);
void dashcdg_nav_settings(lv_disp_t *disp);
void dashcdg_nav_display(lv_disp_t *disp);
void dashcdg_nav_audio_lab(lv_disp_t *disp);
void dashcdg_nav_wifi(lv_disp_t *disp);
void dashcdg_nav_karaoke(lv_disp_t *disp);
