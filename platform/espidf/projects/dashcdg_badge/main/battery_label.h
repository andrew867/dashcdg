/*
 * Shared pack-voltage tint + LVGL battery symbols for status labels (Montserrat ASCII + LV_SYMBOL_*).
 */
#pragma once

#include <stddef.h>

#include "lvgl.h"

/** Pack mV at tint stops: red (at/below LOW) .. blue (by MID) .. green (from MID to FULL). */
#define DASHCDG_BAT_COLOR_MV_LOW  3400
#define DASHCDG_BAT_COLOR_MV_MID  3700
#define DASHCDG_BAT_COLOR_MV_FULL 4150
/** Raw ADC threshold observed near full on this board batch. */
#define DASHCDG_BAT_RAW_FULL 2400

lv_color_t dashcdg_battery_label_color_from_pack_mv(int vbat_mv);

const char *dashcdg_battery_symbol_from_pack_mv(int vbat_mv);
const char *dashcdg_battery_symbol_from_pack_mv_raw(int vbat_mv, int raw_adc);

/** `buf`: LV_SYMBOL + space + "w.fV" (no raw ADC in user strings). */
void dashcdg_battery_format_status_line(char *buf, size_t buf_sz, int vbat_mv);
void dashcdg_battery_format_status_line_raw(char *buf, size_t buf_sz, int vbat_mv, int raw_adc);
