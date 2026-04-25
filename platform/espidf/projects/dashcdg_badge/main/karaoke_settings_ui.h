#pragma once

#include "esp_err.h"

#include "lvgl.h"

/** Setup hub: karaoke decode toggles for Wi-Fi isolation testing. */
esp_err_t dashcdg_karaoke_settings_ui_present(lv_disp_t *disp);
