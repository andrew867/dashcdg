/*
 * GPIO map: Freenove ESP32 CYD 3.2" (240x320 ST7789 + XPT2046), IPS variant.
 * Source: public CYD / E32R32P ESP-IDF demo pin tables (SPI bus shared by LCD + touch).
 * Re-check your silk revision if bring-up fails; adjust mirrors in display_lvgl.c only.
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
#define CYD_GPIO_TP_IRQ        GPIO_NUM_36

#define CYD_LCD_H_RES          240
#define CYD_LCD_V_RES          320

#define CYD_LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

#define CYD_LVGL_BUF_LINES     40

/*
 * ST7789 color pipeline (SPI RGB565 + LVGL): if you see wrong hues (e.g. green/red swap,
 * magenta faces), toggle these — see display_lvgl.c.
 * - SWAP_RGB565_BYTES: almost always 1 on ESP SPI DMA path so LVGL RGB565 matches panel order.
 * - RGB_ELEMENT_ORDER: many ST7789 modules want BGR; try RGB if skin tones look wrong.
 * - PANEL_INVERT: panel inversion command; some batches need false instead of true.
 */
#include "esp_lcd_types.h"
#define CYD_LCD_SWAP_RGB565_BYTES 1
#define CYD_LCD_RGB_ELEMENT_ORDER   LCD_RGB_ELEMENT_ORDER_BGR
#define CYD_LCD_PANEL_INVERT        1

/*
 * XPT2046 → LVGL: corner inversion (e.g. physical top-left hits bottom-right) is fixed by
 * mirroring axes. Tune only if your revision differs.
 */
#define CYD_TP_SWAP_XY   0
#define CYD_TP_MIRROR_X  1
#define CYD_TP_MIRROR_Y  1
