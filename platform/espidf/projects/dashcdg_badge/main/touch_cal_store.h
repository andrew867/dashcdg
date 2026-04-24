#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** NVS namespace shared with Wi-Fi creds; keys prefixed tcal_ */
esp_err_t dashcdg_touch_cal_store_load(uint16_t *x_min, uint16_t *x_max, uint16_t *y_min, uint16_t *y_max);
bool dashcdg_touch_cal_store_has_valid(void);
esp_err_t dashcdg_touch_cal_store_save(uint16_t x_min, uint16_t x_max, uint16_t y_min, uint16_t y_max);

/** Invalidate persisted touch bounds (tcal_ok=0). Wi-Fi keys in namespace dashcfg are untouched. */
esp_err_t dashcdg_touch_cal_store_clear(void);
