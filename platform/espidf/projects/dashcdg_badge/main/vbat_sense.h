#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/**
 * Vbat sense on IO34: classic divider to ADC (see board_cyd_freenove_32.h). Call once at boot
 * (before home UI) so the status bar can read raw + estimated pack voltage. Optional one-point
 * gain trim: `DASHCDG_VBAT_CAL_REF_RAW` / `DASHCDG_VBAT_CAL_REF_BAT_MV` in `board_cyd_freenove_32.h`
 * (set ref raw to 0 to disable). Piecewise ADC tables can be added later if needed.
 */
esp_err_t dashcdg_vbat_sense_init(void);

bool dashcdg_vbat_sense_is_ready(void);

/**
 * Averaged raw count and millivolts. pin_mv is at the sense node; vbat_mv applies Rtop/Rbottom.
 */
esp_err_t dashcdg_vbat_sense_read(int *out_raw, int *out_pin_mv, int *out_vbat_mv);

/** Compact line for a status label, e.g. " 2048 4.1V" or " -- ". */
void dashcdg_vbat_sense_format_line(char *buf, size_t buf_sz);
