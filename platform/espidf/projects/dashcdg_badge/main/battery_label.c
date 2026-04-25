/*
 * Pack-side battery label: red (low) -> blue (mid) -> green (high); symbols from LVGL only.
 */
#include "battery_label.h"

#include <stdio.h>

/** Linear RGB blend: t=0 -> a, t=255 -> b (avoids `lv_color_mix` weight semantics differing by LVGL build). */
static lv_color_t bat_lerp_rgb(lv_color_t a, lv_color_t b, uint16_t t /* 0..255 toward b */)
{
    uint32_t r = (uint32_t)a.red * (255U - t) + (uint32_t)b.red * t;
    uint32_t g = (uint32_t)a.green * (255U - t) + (uint32_t)b.green * t;
    uint32_t bl = (uint32_t)a.blue * (255U - t) + (uint32_t)b.blue * t;
    return lv_color_make((uint8_t)(r / 255U), (uint8_t)(g / 255U), (uint8_t)(bl / 255U));
}

lv_color_t dashcdg_battery_label_color_from_pack_mv(int vbat_mv)
{
    const lv_color_t c_red = lv_color_hex(0xff4444);
    const lv_color_t c_blue = lv_color_hex(0x4488ff);
    const lv_color_t c_green = lv_color_hex(0x33dd66);

    if (vbat_mv <= DASHCDG_BAT_COLOR_MV_LOW) {
        return c_red;
    }
    if (vbat_mv >= DASHCDG_BAT_COLOR_MV_FULL) {
        return c_green;
    }
    if (vbat_mv < DASHCDG_BAT_COLOR_MV_MID) {
        int span = DASHCDG_BAT_COLOR_MV_MID - DASHCDG_BAT_COLOR_MV_LOW;
        int x = span > 0 ? (vbat_mv - DASHCDG_BAT_COLOR_MV_LOW) * 255 / span : 0;
        if (x < 0) {
            x = 0;
        }
        if (x > 255) {
            x = 255;
        }
        return bat_lerp_rgb(c_red, c_blue, (uint16_t)x);
    }
    {
        int span = DASHCDG_BAT_COLOR_MV_FULL - DASHCDG_BAT_COLOR_MV_MID;
        int x = span > 0 ? (vbat_mv - DASHCDG_BAT_COLOR_MV_MID) * 255 / span : 0;
        if (x < 0) {
            x = 0;
        }
        if (x > 255) {
            x = 255;
        }
        return bat_lerp_rgb(c_blue, c_green, (uint16_t)x);
    }
}

const char *dashcdg_battery_symbol_from_pack_mv(int vbat_mv)
{
    return dashcdg_battery_symbol_from_pack_mv_raw(vbat_mv, -1);
}

const char *dashcdg_battery_symbol_from_pack_mv_raw(int vbat_mv, int raw_adc)
{
    if (raw_adc >= DASHCDG_BAT_RAW_FULL || vbat_mv >= DASHCDG_BAT_COLOR_MV_FULL) {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (vbat_mv >= 4050) {
        return LV_SYMBOL_BATTERY_3;
    }
    if (vbat_mv >= 3850) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (vbat_mv >= 3500) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

void dashcdg_battery_format_status_line(char *buf, size_t buf_sz, int vbat_mv)
{
    dashcdg_battery_format_status_line_raw(buf, buf_sz, vbat_mv, -1);
}

void dashcdg_battery_format_status_line_raw(char *buf, size_t buf_sz, int vbat_mv, int raw_adc)
{
    if (!buf || buf_sz < 8) {
        return;
    }
    int deci = (vbat_mv * 10 + 500) / 1000;
    if (deci < 0) {
        deci = 0;
    }
    int w = deci / 10;
    int f = deci % 10;
    snprintf(buf, buf_sz, "%s %d.%dV", dashcdg_battery_symbol_from_pack_mv_raw(vbat_mv, raw_adc), w, f);
}
