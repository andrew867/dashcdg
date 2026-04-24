idf_component_register(
    SRCS
        "main.c"
        "display_lvgl.c"
        "wifi_touch_ui.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_lcd
        esp_timer
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_lvgl_port
        esp_lcd_touch_xpt2046
)
