/*
 * CYD 3.2" — ST7789 + XPT2046 on shared SPI2, backlight GPIO, LVGL via esp_lvgl_port.
 */
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

#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"

static const char *TAG = "disp";

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
        .max_transfer_sz = CYD_LCD_H_RES * CYD_LCD_V_RES * sizeof(uint16_t),
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
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CYD_LCD_HOST, &lcd_io_cfg, &lcd_io), TAG,
        "esp_lcd_new_panel_io_spi lcd");

    esp_lcd_panel_handle_t lcd_panel = NULL;
    esp_lcd_panel_dev_config_t lcd_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(lcd_io, &lcd_cfg, &lcd_panel), TAG, "new_panel_st7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(lcd_panel), TAG, "panel_reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(lcd_panel), TAG, "panel_init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(lcd_panel, true), TAG, "invert_color");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(lcd_panel, true, false), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(lcd_panel, true), TAG, "disp_on");

    gpio_set_level(CYD_GPIO_LCD_BL, 1);

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .buffer_size = CYD_LCD_H_RES * CYD_LVGL_BUF_LINES,
        .double_buffer = true,
        .hres = CYD_LCD_H_RES,
        .vres = CYD_LCD_V_RES,
        .monochrome = false,
        .mipi_dsi = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = false,
            .sw_rotate = false,
            .full_refresh = false,
            .direct_mode = false,
        },
    };

    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp");

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(CYD_GPIO_TP_CS);
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CYD_LCD_HOST, &tp_io_cfg, &tp_io), TAG,
        "esp_lcd_new_panel_io_spi touch");

    esp_lcd_touch_handle_t touch = NULL;
    esp_lcd_touch_config_t touch_cfg = {
        .x_max = CYD_LCD_H_RES,
        .y_max = CYD_LCD_V_RES,
        .rst_gpio_num = -1,
        /* Interrupt optional; polling works for bring-up. Set to CYD_GPIO_TP_IRQ after tuning. */
        .int_gpio_num = -1,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_spi_xpt2046(tp_io, &touch_cfg, &touch), TAG, "touch xpt2046");

    const lvgl_port_touch_cfg_t touch_port_cfg = {
        .disp = disp,
        .handle = touch,
    };
    lv_indev_t *indev = lvgl_port_add_touch(&touch_port_cfg);
    ESP_RETURN_ON_FALSE(indev != NULL, ESP_FAIL, TAG, "lvgl_port_add_touch");

    *out_disp = disp;
    ESP_LOGI(TAG, "display + touch OK");
    return ESP_OK;
}
