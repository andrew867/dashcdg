#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    DASHCDG_HW_SCREEN_HOME = 0,
    DASHCDG_HW_SCREEN_WIFI,
    DASHCDG_HW_SCREEN_SETTINGS,
    DASHCDG_HW_SCREEN_APPLICATIONS,
    DASHCDG_HW_SCREEN_KARAOKE,
    DASHCDG_HW_SCREEN_DISPLAY,
    DASHCDG_HW_SCREEN_AUDIO_LAB,
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

/**
 * Multicast RX task: call after each received UDP datagram (do not hold `badge_rx` mutex).
 * Resets idle dim/sleep timers on the karaoke screen (throttled) and records last RX time so
 * auto-sleep waits until multicast has been quiet for `DASHCDG_HW_IDLE_DIM_MS` (see `board_badge_hw.h`).
 */
void dashcdg_platform_hw_note_karaoke_mcast_rx(uint64_t rx_now_ms);

/**
 * Short soft tone (single sine-enveloped blip) for slider previews etc.
 * Respects touch-beep pref; bumps idle activity. Safe from LVGL thread.
 */
void dashcdg_platform_hw_touch_click(void);

/**
 * "Bep-boo-BEEP" communicator-style triad on buttons and switches only (call from touch indev).
 * Respects touch-beep pref. Wake/sleep jingles are separate and always play from power paths.
 */
void dashcdg_platform_hw_ui_sound_confirm_click(void);

/** Any user interaction (touch, nav, button): resets idle dim/sleep timers and wakes the panel if sleeping. */
void dashcdg_platform_hw_notify_activity(void);

/** Rear RGB status LED (breathing / sleep accent). */
void dashcdg_platform_hw_set_rgb_status_enabled(bool on);
void dashcdg_platform_hw_set_rgb_status_brightness(uint8_t pct_5_100);

/** Idle dim (30s) + sleep (60s) for home / disconnected karaoke. */
void dashcdg_platform_hw_set_auto_sleep_enabled(bool on);

/** Touch LVGL UI tones (button triad, slider preview); off = silent. Power wake/sleep jingles still play. */
void dashcdg_platform_hw_set_touch_beep_enabled(bool on);

/** PWM beep loudness 5-100 (maps to LEDC duty; analog 4k7+20k + SC8002B path is quiet at mid duty). */
void dashcdg_platform_hw_set_beep_volume_pct(uint8_t pct_5_100);

/** PWM lab stream on IO26: fixed ~24 kHz carrier, duty = mono PCM (exclusive with UI beep sequences). */
void dashcdg_platform_hw_lab_pcm_stream_begin(void);
void dashcdg_platform_hw_lab_pcm_stream_end(void);
void dashcdg_platform_hw_lab_pcm_push_u8(uint8_t duty_u8);
uint8_t dashcdg_platform_hw_get_beep_volume_pct(void);

/**
 * LVGL thread: non-zero panel command pending (1 = ST7789 sleep off, 2 = wake on).
 * Call `dashcdg_platform_hw_ack_display_power_cmd()` after a successful `esp_lcd_panel_disp_on_off`.
 */
int dashcdg_platform_hw_peek_display_power_cmd(void);
void dashcdg_platform_hw_ack_display_power_cmd(void);

/**
 * Bits from last `pm_commit_panel_sleep` (consumed once on panel wake, LVGL thread):
 * - `1` → navigate home (slept from settings / display / Wi-Fi);
 * - `2` → resume multicast RX (slept from karaoke; `dashcdg_badge_rx_stop` already ran at sleep commit).
 */
uint32_t dashcdg_platform_hw_consume_post_wake_ui_mask(void);

/**
 * Thread-safe snapshot of the last battery sample taken inside the platform task (~400 ms).
 * Falls back to immediate `dashcdg_vbat_sense_read` if the task is not running.
 */
esp_err_t dashcdg_platform_hw_battery_read(int *out_raw, int *out_pin_mv, int *out_vbat_mv);
