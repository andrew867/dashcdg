set(DASHCDG_BADGE_MAIN_SRCS
        "main.c"
        "badge_exec.c"
        "display_lvgl.c"
        "nav.c"
        "apps_ui.c"
        "home_ui.c"
        "karaoke_ui.c"
        "settings_ui.c"
        "badge_ui_flair.c"
        "display_ui.c"
        "audio_lab_ui.c"
        "audio_settings_ui.c"
        "karaoke_settings_ui.c"
        "badge_lab_ym.c"
        "audio_mgr.c"
        "touch_cal_store.c"
        "touch_cal_ui.c"
        "wifi_touch_ui.c"
        "badge_rx.c"
        "badge_stats.c"
        "badge_cdg_rgb565.c"
        "vbat_sense.c"
        "battery_label.c"
        "badge_prefs.c"
        "platform_hw.c"
)
if(CONFIG_DASHCDG_BADGE_CDG_DEFERRED_BLIT)
    list(APPEND DASHCDG_BADGE_MAIN_SRCS "badge_sp_blit_worker.c")
endif()

idf_component_register(
    SRCS ${DASHCDG_BADGE_MAIN_SRCS}
    INCLUDE_DIRS "."
    REQUIRES
        heap
        esp_hw_support
        esp_app_format
        driver
        esp_driver_dac
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
        dashcdg_rx_audio
        lwip
)

# Match dashcdg_core jitter slot count so sizeof(*s_jb) in this component == core library.
target_compile_definitions(${COMPONENT_LIB} PRIVATE DASHCDG_CDG_BATCH_JITTER_SLOT_COUNT=14)
