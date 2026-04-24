/*
 * CYD 3.2" - ST7789 + XPT2046 on shared SPI2, backlight GPIO, LVGL via esp_lvgl_port.
 *
 * Freenove Arduino samples use tft.setTouch(calData) with raw ADC min/max per axis; the atanisoft
 * driver's default (full 0..4096 -> pixels) exaggerates misalignment when the digitizer extends
 * past the LCD. We disable built-in conversion and map raw ADC with the same bounds (see board
 * header). swap_xy/mirror_y are off; mirror_x may be on (board_cyd_freenove_32.h).
 * dashcdg_touch_process_coordinates maps chip Y-ADC -> LVGL X and chip X-ADC -> LVGL Y, then
 * esp_lcd_touch applies mirror_x; optional CYD_TP_TOUCH_INVERT_LVGL_Y is usually 0.
 *
 * PENIRQ on CYD_GPIO_TP_IRQ + CONFIG_XPT2046_INTERRUPT_MODE (sdkconfig.defaults): chip drives PENIRQ
 * low on touch. esp_lvgl_port switches LVGL to EVENT mode when int_gpio is set; we force TIMER
 * mode after lvgl_port_add_touch so the read timer still polls (releases + cold path).
 */
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "board_cyd_freenove_32.h"
#include "display_lvgl.h"
#include "platform_hw.h"
#include "touch_cal_store.h"

#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"

static const char *TAG = "disp";

/** Minimum Z (pressure) to treat as contact; align with CONFIG_XPT2046_Z_THRESHOLD (~400). */
#define DASHCDG_TOUCH_RAW_Z_THRESH 350

static esp_lcd_touch_handle_t s_tp;
static lv_indev_t *s_touch_indev;
static volatile bool s_touch_raw_mode;

/** After panel wake, drop stale pen-down so the first mapped point does not click UI. */
static lv_timer_t *s_post_wake_touch_timer;

static void post_wake_touch_timer_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    s_post_wake_touch_timer = NULL;
    /* LVGL timer context: avoid nesting `lvgl_port_lock` (mutex is often already held). */
    dashcdg_touch_rearm_locked();
}

static void dashcdg_display_schedule_post_wake_touch_rearm(void)
{
    if (s_post_wake_touch_timer) {
        lv_timer_del(s_post_wake_touch_timer);
        s_post_wake_touch_timer = NULL;
    }
    s_post_wake_touch_timer = lv_timer_create(post_wake_touch_timer_cb, 420, NULL);
}

/** Same panel handle passed to esp_lvgl_port (LVGL logical coords == draw_bitmap after MADCTL). */
static esp_lcd_panel_handle_t s_lcd_panel;

static void dashcdg_indev_activity_on_press(lv_event_t *e)
{
    (void)e;
    dashcdg_platform_hw_notify_activity();
}

static void dashcdg_indev_hw_feedback(lv_event_t *e)
{
    (void)e;
    dashcdg_platform_hw_touch_click();
}

static void dashcdg_panel_power_lv_cb(lv_timer_t *t)
{
    (void)t;
    dashcdg_display_lvgl_poll_panel_power();
}

void dashcdg_display_lvgl_poll_panel_power(void)
{
    int cmd = dashcdg_platform_hw_peek_display_power_cmd();
    if (cmd == 0 || s_lcd_panel == NULL) {
        return;
    }
    if (!lvgl_port_lock(120)) {
        return;
    }
    (void)esp_lcd_panel_disp_on_off(s_lcd_panel, cmd == 2);
    if (cmd == 2) {
        dashcdg_display_schedule_post_wake_touch_rearm();
    }
    lvgl_port_unlock();
    dashcdg_platform_hw_ack_display_power_cmd();
}

static int32_t s_tp_xmin = CYD_TP_RAW_X_MIN;
static int32_t s_tp_xmax = CYD_TP_RAW_X_MAX;
static int32_t s_tp_ymin = CYD_TP_RAW_Y_MIN;
static int32_t s_tp_ymax = CYD_TP_RAW_Y_MAX;

/** Map one axis from raw ADC to 0..out_max-1 (matches TFT_eSPI linear touch cal). */
static uint16_t map_adc_axis(uint16_t raw, int32_t raw_min, int32_t raw_max, int32_t out_max)
{
    if (out_max <= 1) {
        return 0;
    }
    if (raw_max <= raw_min) {
        return 0;
    }
    int32_t r = (int32_t)raw;
    if (r < raw_min) {
        r = raw_min;
    }
    if (r > raw_max) {
        r = raw_max;
    }
    int32_t span = raw_max - raw_min;
    int32_t x = (r - raw_min) * (out_max - 1) / span;
    if (x < 0) {
        x = 0;
    }
    if (x >= out_max) {
        x = out_max - 1;
    }
    return (uint16_t)x;
}

/**
 * Called by esp_lcd_touch before optional swap/mirror (see esp_lcd_touch.c).
 * XPT2046 returns chip X register in x[], Y register in y[]. On this CYD stack, landscape LVGL X
 * follows the resistive Y measurement and LVGL Y follows X - map to correct output sizes here;
 * do not use esp_lcd_touch swap_xy for that (it only swaps two numbers with mismatched ranges).
 */
static void dashcdg_touch_process_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                                            uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    (void)max_point_num;
    if (!point_num || *point_num == 0) {
        return;
    }
    if (s_touch_raw_mode) {
        return;
    }
    for (uint8_t i = 0; i < *point_num; i++) {
        const uint16_t ax = x[i];
        const uint16_t ay = y[i];
        uint16_t lv_x = map_adc_axis(ay, s_tp_ymin, s_tp_ymax, CYD_LCD_H_RES);
        uint16_t lv_y = map_adc_axis(ax, s_tp_xmin, s_tp_xmax, CYD_LCD_V_RES);
#if CYD_TP_TOUCH_INVERT_LVGL_Y
        lv_y = (uint16_t)((CYD_LCD_V_RES - 1) - (int32_t)lv_y);
#endif
        x[i] = lv_x;
        y[i] = lv_y;
    }
}

void dashcdg_touch_set_calibration_adc(uint16_t x_min, uint16_t x_max, uint16_t y_min, uint16_t y_max)
{
    s_tp_xmin = x_min;
    s_tp_xmax = x_max;
    s_tp_ymin = y_min;
    s_tp_ymax = y_max;
}

/** NVS can hold tcal_ok=1 with garbage spans (e.g. partial write); that maps every point off-widgets. */
static bool touch_cal_adc_spans_sane(uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1)
{
    const uint32_t min_span = 48;
    uint32_t sx = (x0 < x1) ? (uint32_t)(x1 - x0) : (uint32_t)(x0 - x1);
    uint32_t sy = (y0 < y1) ? (uint32_t)(y1 - y0) : (uint32_t)(y0 - y1);
    return sx >= min_span && sy >= min_span;
}

esp_err_t dashcdg_touch_apply_store_or_defaults(void)
{
    uint16_t x0, x1, y0, y1;
    esp_err_t e = dashcdg_touch_cal_store_load(&x0, &x1, &y0, &y1);
    if (e == ESP_OK && touch_cal_adc_spans_sane(x0, x1, y0, y1)) {
        dashcdg_touch_set_calibration_adc(x0, x1, y0, y1);
        ESP_LOGI(TAG, "touch cal from NVS: X %u..%u  Y %u..%u", x0, x1, y0, y1);
        return ESP_OK;
    }
    if (e == ESP_OK) {
        ESP_LOGW(TAG,
                 "touch cal NVS failed sanity (span); using board defaults - re-run touch cal or erase namespace dashcfg");
    }
    dashcdg_touch_set_calibration_adc(CYD_TP_RAW_X_MIN, CYD_TP_RAW_X_MAX, CYD_TP_RAW_Y_MIN, CYD_TP_RAW_Y_MAX);
    ESP_LOGI(TAG, "touch cal: board defaults (no NVS or rejected)");
    return e;
}

void dashcdg_display_clear_top_layer(lv_disp_t *disp)
{
    if (!disp) {
        return;
    }
    lv_obj_t *top = lv_display_get_layer_top(disp);
    if (top) {
        lv_obj_clean(top);
    }
}

void dashcdg_touch_input_enable(bool enable)
{
    if (s_touch_indev) {
        lv_indev_enable(s_touch_indev, enable);
    }
}

void dashcdg_touch_rearm_locked(void)
{
    if (!s_touch_indev) {
        return;
    }
    lv_indev_enable(s_touch_indev, true);
    lv_indev_reset(s_touch_indev, NULL);
}

/*
 * If NVS cal is active but mapped (x,y) stay in a pinhole while Z swings (finger moving / varying
 * pressure), the stored ADC bounds are almost certainly wrong - clear NVS and use board defaults.
 */
#define STUCK_CAL_RING 36
#define STUCK_CAL_MIN_SAMPLES 22
#define STUCK_CAL_Z_SPAN_MIN 110
#define STUCK_CAL_XY_SPAN_MAX 14

static uint16_t s_stuck_ring_x[STUCK_CAL_RING];
static uint16_t s_stuck_ring_y[STUCK_CAL_RING];
static uint16_t s_stuck_ring_z[STUCK_CAL_RING];
static uint8_t s_stuck_ring_count;

static void stuck_cal_ring_reset(void)
{
    s_stuck_ring_count = 0;
}

static void stuck_cal_ring_push(uint16_t x, uint16_t y, uint16_t z)
{
    if (s_stuck_ring_count < STUCK_CAL_RING) {
        s_stuck_ring_x[s_stuck_ring_count] = x;
        s_stuck_ring_y[s_stuck_ring_count] = y;
        s_stuck_ring_z[s_stuck_ring_count] = z;
        s_stuck_ring_count++;
    } else {
        for (uint8_t i = 1; i < STUCK_CAL_RING; i++) {
            s_stuck_ring_x[i - 1] = s_stuck_ring_x[i];
            s_stuck_ring_y[i - 1] = s_stuck_ring_y[i];
            s_stuck_ring_z[i - 1] = s_stuck_ring_z[i];
        }
        s_stuck_ring_x[STUCK_CAL_RING - 1] = x;
        s_stuck_ring_y[STUCK_CAL_RING - 1] = y;
        s_stuck_ring_z[STUCK_CAL_RING - 1] = z;
    }
}

/** @return true if NVS cal was cleared this call */
static bool stuck_cal_maybe_invalidate_nvs(uint16_t x, uint16_t y, uint16_t z)
{
    stuck_cal_ring_push(x, y, z);
    if (s_stuck_ring_count < STUCK_CAL_MIN_SAMPLES || !dashcdg_touch_cal_store_has_valid()) {
        return false;
    }
    uint16_t xmin = x, xmax = x, ymin = y, ymax = y, zmin = z, zmax = z;
    for (uint8_t i = 0; i < s_stuck_ring_count; i++) {
        uint16_t xi = s_stuck_ring_x[i];
        uint16_t yi = s_stuck_ring_y[i];
        uint16_t zi = s_stuck_ring_z[i];
        if (xi < xmin) {
            xmin = xi;
        }
        if (xi > xmax) {
            xmax = xi;
        }
        if (yi < ymin) {
            ymin = yi;
        }
        if (yi > ymax) {
            ymax = yi;
        }
        if (zi < zmin) {
            zmin = zi;
        }
        if (zi > zmax) {
            zmax = zi;
        }
    }
    uint32_t zspan = (uint32_t)zmax - (uint32_t)zmin;
    uint32_t xspan = (uint32_t)xmax - (uint32_t)xmin;
    uint32_t yspan = (uint32_t)ymax - (uint32_t)ymin;
    if (zspan < STUCK_CAL_Z_SPAN_MIN || xspan > STUCK_CAL_XY_SPAN_MAX || yspan > STUCK_CAL_XY_SPAN_MAX) {
        return false;
    }
    esp_err_t cl = dashcdg_touch_cal_store_clear();
    if (cl != ESP_OK) {
        ESP_LOGW(TAG, "touch stuck-cal: NVS clear failed: %s", esp_err_to_name(cl));
        stuck_cal_ring_reset();
        return false;
    }
    (void)dashcdg_touch_apply_store_or_defaults();
    ESP_LOGW(TAG,
             "touch: cleared NVS cal (Z span=%lu mapped XY span %lu x %lu) - using board defaults; re-run cal if needed",
             (unsigned long)zspan, (unsigned long)xspan, (unsigned long)yspan);
    stuck_cal_ring_reset();
    /* Overlay runs from an LVGL timer; same context as other lv_* touch calls - rearm without nesting port lock. */
    dashcdg_touch_rearm_locked();
    return true;
}

void dashcdg_touch_debug_format_line(char *buf, size_t buf_sz)
{
    if (!buf || buf_sz < 8) {
        return;
    }
    buf[0] = '\0';
    if (!s_tp) {
        snprintf(buf, buf_sz, "tp=(null)");
        return;
    }

    esp_err_t e1 = esp_lcd_touch_read_data(s_tp);
    esp_lcd_touch_point_data_t pts[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint8_t cnt = 0;
    esp_err_t e2 = esp_lcd_touch_get_data(s_tp, pts, &cnt, CONFIG_ESP_LCD_TOUCH_MAX_POINTS);

    /* Runtime only: GPIO_NUM_* are enums - #if (CYD_GPIO_TP_IRQ != GPIO_NUM_NC) is unreliable in cpp. */
    int irq = -1;
    if (CYD_GPIO_TP_IRQ != GPIO_NUM_NC) {
        irq = gpio_get_level(CYD_GPIO_TP_IRQ);
    }

    if (e1 != ESP_OK || e2 != ESP_OK) {
        snprintf(buf, buf_sz, "rd=%s get=%s irq=%d", esp_err_to_name(e1), esp_err_to_name(e2), irq);
        return;
    }
    if (cnt == 0) {
        stuck_cal_ring_reset();
        snprintf(buf, buf_sz, "n=0 z=- irq=%d (SPI ok, no contact)", irq);
        return;
    }
    bool cleared = stuck_cal_maybe_invalidate_nvs(pts[0].x, pts[0].y, pts[0].strength);
    if (cleared) {
        snprintf(buf, buf_sz, "n=%u x=%u y=%u z=%u irq=%d | NVS cal cleared->defaults", (unsigned)cnt,
                 (unsigned)pts[0].x, (unsigned)pts[0].y, (unsigned)pts[0].strength, irq);
    } else {
        snprintf(buf, buf_sz, "n=%u x=%u y=%u z=%u irq=%d [LVGL map]", (unsigned)cnt, (unsigned)pts[0].x,
                 (unsigned)pts[0].y, (unsigned)pts[0].strength, irq);
    }
}

bool dashcdg_touch_read_raw_adc(uint16_t *raw_x, uint16_t *raw_y, bool *pressed)
{
    if (!s_tp || raw_x == NULL || raw_y == NULL || pressed == NULL) {
        return false;
    }

    bool swap, mx, my;
    esp_lcd_touch_get_swap_xy(s_tp, &swap);
    esp_lcd_touch_get_mirror_x(s_tp, &mx);
    esp_lcd_touch_get_mirror_y(s_tp, &my);

    esp_lcd_touch_set_swap_xy(s_tp, false);
    esp_lcd_touch_set_mirror_x(s_tp, false);
    esp_lcd_touch_set_mirror_y(s_tp, false);

    s_touch_raw_mode = true;

    esp_lcd_touch_read_data(s_tp);
    esp_lcd_touch_point_data_t pt;
    uint8_t n = 0;
    esp_err_t gd = esp_lcd_touch_get_data(s_tp, &pt, &n, 1);

    s_touch_raw_mode = false;

    esp_lcd_touch_set_swap_xy(s_tp, swap);
    esp_lcd_touch_set_mirror_x(s_tp, mx);
    esp_lcd_touch_set_mirror_y(s_tp, my);

    if (gd == ESP_OK && n > 0 && pt.strength >= DASHCDG_TOUCH_RAW_Z_THRESH) {
        *raw_x = pt.x;
        *raw_y = pt.y;
        *pressed = true;
    } else {
        *pressed = false;
    }
    return true;
}

esp_err_t dashcdg_display_lvgl_init(lv_disp_t **out_disp)
{
    ESP_RETURN_ON_FALSE(out_disp != NULL, ESP_ERR_INVALID_ARG, TAG, "out_disp");

    gpio_config_t bk = {
        .pin_bit_mask = 1ULL << CYD_GPIO_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bk), TAG, "gpio_config BL");
    gpio_set_level(CYD_GPIO_LCD_BL, 0);

    spi_bus_config_t bus = {
        .mosi_io_num = CYD_GPIO_LCD_MOSI,
        .miso_io_num = CYD_GPIO_LCD_MISO,
        .sclk_io_num = CYD_GPIO_LCD_PCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CYD_LCD_PHYS_W * CYD_LCD_PHYS_H * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(CYD_LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi_bus_initialize");

    esp_lcd_panel_io_handle_t lcd_io = NULL;
    esp_lcd_panel_io_spi_config_t lcd_io_cfg = {
        .dc_gpio_num = CYD_GPIO_LCD_DC,
        .cs_gpio_num = CYD_GPIO_LCD_CS,
        .pclk_hz = CYD_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        /* Keep single in-flight transfer: CDG overlay reuses one band scratch buffer. */
        .trans_queue_depth = 1,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CYD_LCD_HOST, &lcd_io_cfg, &lcd_io), TAG,
        "esp_lcd_new_panel_io_spi lcd");

    esp_lcd_panel_handle_t lcd_panel = NULL;
    esp_lcd_panel_dev_config_t lcd_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = CYD_LCD_RGB_ELEMENT_ORDER,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(lcd_io, &lcd_cfg, &lcd_panel), TAG, "new_panel_st7789");
    s_lcd_panel = lcd_panel;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(lcd_panel), TAG, "panel_reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(lcd_panel), TAG, "panel_init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(lcd_panel, CYD_LCD_PANEL_INVERT), TAG, "invert_color");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(lcd_panel, true), TAG, "disp_on");

    gpio_set_level(CYD_GPIO_LCD_BL, 1);

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .control_handle = NULL,
        .buffer_size = CYD_LCD_H_RES * CYD_LVGL_BUF_LINES,
        .double_buffer = (CYD_LVGL_DOUBLE_BUFFER != 0),
        .trans_size = 0,
        .hres = CYD_LCD_H_RES,
        .vres = CYD_LCD_V_RES,
        .monochrome = false,
        /* Landscape: logical 320x240 on native 240x320 panel (see board_cyd_freenove_32.h). */
        .rotation = {
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .rounder_cb = NULL,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            /* SPI RGB565: match LVGL flush to panel (see CYD_LCD_SWAP_RGB565_BYTES in board header). */
            .swap_bytes = CYD_LCD_SWAP_RGB565_BYTES,
            .full_refresh = CYD_LVGL_FULL_REFRESH,
            .direct_mode = 0,
        },
    };

    lv_disp_t *disp = (lv_disp_t *)lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp");

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(CYD_GPIO_TP_CS);
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CYD_LCD_HOST, &tp_io_cfg, &tp_io), TAG,
        "esp_lcd_new_panel_io_spi touch");

    esp_lcd_touch_handle_t touch = NULL;
    esp_lcd_touch_config_t touch_cfg = {0};
    touch_cfg.x_max = CYD_LCD_H_RES;
    touch_cfg.y_max = CYD_LCD_V_RES;
    touch_cfg.rst_gpio_num = GPIO_NUM_NC;
    touch_cfg.int_gpio_num = CYD_GPIO_TP_IRQ;
    touch_cfg.levels.interrupt = 0; /* PENIRQ active low -> NEGEDGE before esp_lcd_touch registers ISR */
    touch_cfg.flags.swap_xy = CYD_TP_SWAP_XY;
    touch_cfg.flags.mirror_x = CYD_TP_MIRROR_X;
    touch_cfg.flags.mirror_y = CYD_TP_MIRROR_Y;
    touch_cfg.process_coordinates = dashcdg_touch_process_coordinates;

    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_spi_xpt2046(tp_io, &touch_cfg, &touch), TAG, "touch xpt2046");

    const lvgl_port_touch_cfg_t touch_port_cfg = {
        .disp = disp,
        .handle = touch,
    };
    lv_indev_t *indev = lvgl_port_add_touch(&touch_port_cfg);
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_FAIL, TAG, "lvgl_port_add_touch");

    s_tp = touch;
    s_touch_indev = indev;
    lv_indev_add_event_cb(indev, dashcdg_indev_activity_on_press, LV_EVENT_PRESSED, NULL);
    lv_indev_add_event_cb(indev, dashcdg_indev_hw_feedback, LV_EVENT_CLICKED, NULL);
    (void)dashcdg_touch_apply_store_or_defaults();

    (void)lv_timer_create(dashcdg_panel_power_lv_cb, 150, NULL);

    lvgl_port_lock(0);
    lv_display_set_default(disp);
    if (indev && touch_cfg.int_gpio_num != GPIO_NUM_NC) {
        lv_indev_set_mode(indev, LV_INDEV_MODE_TIMER);
    }
    dashcdg_touch_rearm_locked();
    lvgl_port_unlock();

    *out_disp = disp;
    ESP_LOGI(TAG, "display + touch OK");
    return ESP_OK;
}

esp_lcd_panel_handle_t dashcdg_display_lcd_panel(void)
{
    return s_lcd_panel;
}

/** Match `esp_lvgl_port` RGB565 path: BGR element order + per-pixel byte swap before SPI (board_cyd_freenove_32.h). */
static void dashcdg_display_pack_rgb565_buffer_for_panel(uint16_t *buf, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        uint16_t c = buf[i];

#if CYD_LCD_RGB_ELEMENT_ORDER == LCD_RGB_ELEMENT_ORDER_BGR
        /* Classic RGB565 -> BGR565 (swap 5-bit R and B fields; G stays in the middle). */
        c = (uint16_t)(((c & 0xF800U) >> 11) | (c & 0x07E0U) | ((c & 0x001FU) << 11));
#endif
#if CYD_LCD_SWAP_RGB565_BYTES
        c = (uint16_t)((c >> 8) | (c << 8));
#endif
        buf[i] = c;
    }
}

esp_err_t dashcdg_display_blit_rgb565_lv_area(int x0, int y0, int w, int h, uint16_t *pixels)
{
    size_t n;

    if (s_lcd_panel == NULL || pixels == NULL || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    n = (size_t)w * (size_t)h;
    dashcdg_display_pack_rgb565_buffer_for_panel(pixels, n);
    return esp_lcd_panel_draw_bitmap(s_lcd_panel, x0, y0, x0 + w, y0 + h, pixels);
}
