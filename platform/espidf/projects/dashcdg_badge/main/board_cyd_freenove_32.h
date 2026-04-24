/*
 * GPIO map: Freenove ESP32 CYD 3.2" (240x320 ST7789 + XPT2046), IPS variant.
 * LVGL uses landscape logical coordinates (320x240); panel is rotated in the LCD driver.
 * Source: public CYD / E32R32P ESP-IDF demo pin tables (SPI bus shared by LCD + touch).
 * Re-check your silk revision if bring-up fails; tune TP mirrors / swap_xy if touch drifts.
 */
#pragma once

#include "driver/gpio.h"

#define CYD_LCD_HOST           SPI2_HOST

#define CYD_GPIO_LCD_MOSI      GPIO_NUM_13
#define CYD_GPIO_LCD_MISO      GPIO_NUM_12
#define CYD_GPIO_LCD_PCLK      GPIO_NUM_14
#define CYD_GPIO_LCD_DC        GPIO_NUM_2
#define CYD_GPIO_LCD_CS        GPIO_NUM_15
#define CYD_GPIO_LCD_RST       GPIO_NUM_NC
#define CYD_GPIO_LCD_BL        GPIO_NUM_27

#define CYD_GPIO_TP_CS         GPIO_NUM_33
/* XPT2046 PENIRQ (active low). ESP32 VP/IO36 has no usable internal pull-up; this board uses ~10k to 3V3. */
#define CYD_GPIO_TP_IRQ        GPIO_NUM_36

/*
 * Battery sense (ADC1) - 100k from Vbat to node, 100k node to GND (Vpin = Vbat * Rb / (Rt+Rb)).
 * IO34: input-only, suits ADC. "Full" cal point: note raw + pack V when TP4054 hits end-of-charge
 * (common ~4.2 V cell w/ 3.3k PROG on TP4054/4056 class CC/CV, ~0.1C termination); "empty" TBD
 * (e.g. at brownout). Tweak Rtop/Rbottom if your build differs.
 */
#define CYD_GPIO_VBAT_SENSE   GPIO_NUM_34
#define CYD_VBAT_R_OHM_TOP    100000u
#define CYD_VBAT_R_OHM_BOTTOM 100000u

/*
 * One-point pack voltage trim (see vbat_sense.c): at known averaged raw, true pack mV (DMM).
 * Applies a constant gain so that raw matches the reference. Set DASHCDG_VBAT_CAL_REF_RAW to 0
 * to disable. Add more points later if you extend vbat_sense to a piecewise table.
 */
#ifndef DASHCDG_VBAT_CAL_REF_RAW
#define DASHCDG_VBAT_CAL_REF_RAW 2100
#endif
#ifndef DASHCDG_VBAT_CAL_REF_BAT_MV
#define DASHCDG_VBAT_CAL_REF_BAT_MV 3620
#endif

/* Physical panel pixels (before rotation). */
#define CYD_LCD_PHYS_W         240
#define CYD_LCD_PHYS_H         320

/* LVGL resolution: landscape (matches CDG / protocol UI orientation). */
#define CYD_LCD_H_RES          320
#define CYD_LCD_V_RES          240

#define CYD_LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

/*
 * LVGL partial-draw RAM (internal DMA): total ~ CYD_LCD_H_RES * CYD_LVGL_BUF_LINES * 2 bytes x (1 or 2 buffers).
 * Default is lean for ESP32 internal DRAM (CDG state ~65 KiB on heap). Raise lines / double-buffer for smoother UI.
 */
#ifndef CYD_LVGL_BUF_LINES
#define CYD_LVGL_BUF_LINES 16
#endif
#ifndef CYD_LVGL_DOUBLE_BUFFER
#define CYD_LVGL_DOUBLE_BUFFER 0
#endif

/*
 * Full-frame redraw: leave 0 on ESP32 (was contributing to WDT / unstable runs). Toggle 1 only
 * briefly if you need to debug IPS ghosting and can afford the CPU cost.
 */
#ifndef CYD_LVGL_FULL_REFRESH
#define CYD_LVGL_FULL_REFRESH  0
#endif

/*
 * ST7789 color pipeline (SPI RGB565 + LVGL): if you see wrong hues (e.g. green/red swap,
 * magenta faces), toggle these - see display_lvgl.c.
 * - SWAP_RGB565_BYTES: almost always 1 on ESP SPI DMA path so LVGL RGB565 matches panel order.
 * - RGB_ELEMENT_ORDER: many ST7789 modules want BGR; try RGB if skin tones look wrong.
 * - PANEL_INVERT: panel inversion command; some batches need false instead of true.
 * Wrong primaries (e.g. yellow vs magenta): toggle only one of SWAP_RGB565_BYTES or
 * RGB_ELEMENT_ORDER at a time vs LVGL + CDG both use display_lvgl.c packing.
 */
#include "esp_lcd_types.h"
#define CYD_LCD_SWAP_RGB565_BYTES 1
#define CYD_LCD_RGB_ELEMENT_ORDER   LCD_RGB_ELEMENT_ORDER_BGR
#define CYD_LCD_PANEL_INVERT        1

/*
 * XPT2046 -> LVGL (landscape), verified on Freenove CYD 3.2" IPS:
 * - Do not use esp_lcd_touch swap_xy to pair axes (see display_lvgl.c).
 * - Software mirror X in esp_lcd_touch is ON for this stack.
 * - CYD_TP_TOUCH_INVERT_LVGL_Y is OFF; use mirror_x + mapping instead of post-map Y invert.
 */
#define CYD_TP_SWAP_XY    0
#define CYD_TP_MIRROR_X   1
#define CYD_TP_MIRROR_Y   0
/* Optional: flip LVGL Y after chip-X->LVGL-Y map (normally 0 when mirror_x handles orientation). */
#ifndef CYD_TP_TOUCH_INVERT_LVGL_Y
#define CYD_TP_TOUCH_INVERT_LVGL_Y 0
#endif

/*
 * TFT_eSPI-style raw ADC bounds for the 3.2" Freenove stack (same role as setTouch(calData)).
 * From third_party/Freenove_ESP32_Display/.../Sketch_12.2_TFT_Touch_Draw_3.2_Inch.ino:
 *   uint16_t calData[5] = { 412, 3502, 262, 3596, 3 }; // tft.setTouch(calData);
 * Touch foil is slightly larger than the visible LCD; mapping full 0..4095 to pixels exaggerates
 * that - use these mins/maxs with CONFIG_XPT2046_CONVERT_ADC_TO_COORDS=n + process_coordinates.
 */
#ifndef CYD_TP_RAW_X_MIN
#define CYD_TP_RAW_X_MIN 412
#endif
#ifndef CYD_TP_RAW_X_MAX
#define CYD_TP_RAW_X_MAX 3502
#endif
#ifndef CYD_TP_RAW_Y_MIN
#define CYD_TP_RAW_Y_MIN 262
#endif
#ifndef CYD_TP_RAW_Y_MAX
#define CYD_TP_RAW_Y_MAX 3596
#endif
