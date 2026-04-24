#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

/** ST7789 + XPT2046 + esp_lvgl_port (FreeRTOS). Touch axis map lives in display_lvgl.c + board_cyd_freenove_32.h. */
esp_err_t dashcdg_display_lvgl_init(lv_disp_t **out_disp);

/**
 * LVGL-thread hook: apply panel sleep/wake requested by `platform_hw` (call from an LVGL timer).
 * Uses `lvgl_port_lock` + `esp_lcd_panel_disp_on_off`.
 */
void dashcdg_display_lvgl_poll_panel_power(void);

/** Panel used by LVGL (same MADCTL as port). NULL before init. */
esp_lcd_panel_handle_t dashcdg_display_lcd_panel(void);

/**
 * Blit RGB565 pixels into LVGL logical coordinates (same space as lv_obj positions after port rotation).
 * Row-major, w * h pixels; typically used with a small band buffer for CDG.
 * Buffer is adjusted in-place for ST7789 BGR + SPI byte order (see CYD_LCD_* in board header), then drawn.
 */
esp_err_t dashcdg_display_blit_rgb565_lv_area(int x0, int y0, int w, int h, uint16_t *pixels);

/** Remove all children from the display top layer (orphans here receive pointer hits before the active screen). */
void dashcdg_display_clear_top_layer(lv_disp_t *disp);

/** Linear ADC bounds for X/Y (same as TFT_eSPI calData[0..3]); runtime, loaded from NVS when present. */
void dashcdg_touch_set_calibration_adc(uint16_t x_min, uint16_t x_max, uint16_t y_min, uint16_t y_max);
esp_err_t dashcdg_touch_apply_store_or_defaults(void);

/** Disable LVGL pointer input while sampling raw ADC (touch calibration wizard). */
void dashcdg_touch_input_enable(bool enable);

/**
 * Re-enable pointer input and drop stale press/hover state. Call only with `lvgl_port_lock` held
 * (same rules as other `lv_*` UI calls).
 */
void dashcdg_touch_rearm_locked(void);

/**
 * Sample raw XPT2046 channels (rotation flags cleared; no linear mapping).
 * Call only while LVGL touch input is disabled.
 */
bool dashcdg_touch_read_raw_adc(uint16_t *raw_x, uint16_t *raw_y, bool *pressed);

/**
 * One SPI read + get_data snapshot for on-screen debug (post-process_coordinates LVGL x/y, strength).
 * Safe to call from an LVGL timer; uses same controller as the pointer indev.
 */
void dashcdg_touch_debug_format_line(char *buf, size_t buf_sz);
