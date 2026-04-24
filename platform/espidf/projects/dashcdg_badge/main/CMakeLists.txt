idf_component_register(
    SRCS
        "main.c"
        "display_lvgl.c"
        "nav.c"
        "home_ui.c"
        "karaoke_ui.c"
        "settings_ui.c"
        "touch_cal_store.c"
        "touch_cal_ui.c"
        "wifi_touch_ui.c"
        "badge_rx.c"
        "badge_cdg_rgb565.c"
        "vbat_sense.c"
    INCLUDE_DIRS "."
    REQUIRES
        heap
        esp_hw_support
        esp_app_format
        driver
        esp_adc
        esp_lcd
        esp_timer
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_lvgl_port
        esp_lcd_touch_xpt2046
        dashcdg_core
        dashcdg_proto
        lwip
)

# Match dashcdg_core jitter slot count so sizeof(*s_jb) in this component == core library.
target_compile_definitions(${COMPONENT_LIB} PRIVATE DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT=16)
