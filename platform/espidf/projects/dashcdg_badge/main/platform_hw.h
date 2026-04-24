#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    DASHCDG_HW_SCREEN_HOME = 0,
    DASHCDG_HW_SCREEN_WIFI,
    DASHCDG_HW_SCREEN_SETTINGS,
    DASHCDG_HW_SCREEN_KARAOKE,
    DASHCDG_HW_SCREEN_DISPLAY,
} dashcdg_hw_screen_t;

/**
 * Start the low-priority "platform" task: RGB status LED, backlight PWM, battery cache, IO0 button,
 * amp GPIO, I2C bus init, and PWM beep on IO26. Call after `dashcdg_display_lvgl_init` and
 * `dashcdg_vbat_sense_init` (Vbat may also be initialized here if not done yet).
 */
esp_err_t dashcdg_platform_hw_init(void);

bool dashcdg_platform_hw_is_ready(void);

/** Backlight 0-100 (%). High-side IO27: higher pct = brighter. */
void dashcdg_platform_hw_backlight_set_pct(uint8_t pct_0_100);

/** Where the UI is - drives RGB breathing / heartbeat personality. */
void dashcdg_platform_hw_set_screen(dashcdg_hw_screen_t s);

/**
 * Karaoke / RX path: call from UI tick when multicast CDG looks alive (clock + deltas pending).
 * Feeds a faster "stream OK" LED personality while on the karaoke screen.
 */
void dashcdg_platform_hw_set_cdg_stream_ok(bool ok);

/** Short UI tick for touch hardware validation (safe from LVGL thread). */
void dashcdg_platform_hw_touch_click(void);

/** Any user interaction (touch, nav, button): resets idle dim/sleep timers and wakes the panel if sleeping. */
void dashcdg_platform_hw_notify_activity(void);

/** Rear RGB status LED (breathing / sleep accent). */
void dashcdg_platform_hw_set_rgb_status_enabled(bool on);
void dashcdg_platform_hw_set_rgb_status_brightness(uint8_t pct_5_100);

/** Idle dim (30s) + sleep (60s) for home / disconnected karaoke. */
void dashcdg_platform_hw_set_auto_sleep_enabled(bool on);

/** Touch LVGL click tick (IO26 PWM); off = silent taps. IO0 wake/sleep tones unchanged. */
void dashcdg_platform_hw_set_touch_beep_enabled(bool on);

/** PWM beep loudness 5-100 (maps to LEDC duty; analog 4k7+20k + SC8002B path is quiet at mid duty). */
void dashcdg_platform_hw_set_beep_volume_pct(uint8_t pct_5_100);

/**
 * LVGL thread: non-zero panel command pending (1 = ST7789 sleep off, 2 = wake on).
 * Call `dashcdg_platform_hw_ack_display_power_cmd()` after a successful `esp_lcd_panel_disp_on_off`.
 */
int dashcdg_platform_hw_peek_display_power_cmd(void);
void dashcdg_platform_hw_ack_display_power_cmd(void);

/**
 * Thread-safe snapshot of the last battery sample taken inside the platform task (~400 ms).
 * Falls back to immediate `dashcdg_vbat_sense_read` if the task is not running.
 */
esp_err_t dashcdg_platform_hw_battery_read(int *out_raw, int *out_pin_mv, int *out_vbat_mv);
