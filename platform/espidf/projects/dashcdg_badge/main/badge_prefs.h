#pragma once

#include <stdint.h>

#include "esp_err.h"

/** NVS namespace `dashcfg` (shared with touch cal). */
esp_err_t dashcdg_badge_prefs_load_brightness(uint8_t *out_pct_5_100);
esp_err_t dashcdg_badge_prefs_save_brightness(uint8_t pct_5_100);

/** Rear RGB status LED: `out_on` 0/1 (default on), `out_pct` 5-100 (default 100). */
esp_err_t dashcdg_badge_prefs_load_rgb_status(uint8_t *out_on, uint8_t *out_pct_5_100);
esp_err_t dashcdg_badge_prefs_save_rgb_status(uint8_t on, uint8_t pct_5_100);

/** Auto dim/sleep on idle: `out_on` 0/1 (default on). */
esp_err_t dashcdg_badge_prefs_load_auto_sleep(uint8_t *out_on);
esp_err_t dashcdg_badge_prefs_save_auto_sleep(uint8_t on);

/** PWM touch beep level on IO26 (5-100 % duty scale; default 85). */
esp_err_t dashcdg_badge_prefs_load_beep_volume(uint8_t *out_pct_5_100);
esp_err_t dashcdg_badge_prefs_save_beep_volume(uint8_t pct_5_100);

/** Touch click beep: `out_on` 0/1 (default on). */
esp_err_t dashcdg_badge_prefs_load_touch_beep(uint8_t *out_on);
esp_err_t dashcdg_badge_prefs_save_touch_beep(uint8_t on);

/** Karaoke decode toggles: defaults on. */
esp_err_t dashcdg_badge_prefs_load_karaoke_video_decode(uint8_t *out_on);
esp_err_t dashcdg_badge_prefs_save_karaoke_video_decode(uint8_t on);
esp_err_t dashcdg_badge_prefs_load_karaoke_audio_decode(uint8_t *out_on);
esp_err_t dashcdg_badge_prefs_save_karaoke_audio_decode(uint8_t on);

/** v4 CDG repair: request parity retransmit from TX (default on). */
esp_err_t dashcdg_badge_prefs_load_karaoke_repair_nack(uint8_t *out_on);
esp_err_t dashcdg_badge_prefs_save_karaoke_repair_nack(uint8_t on);
/** v4_rx_stats uplink to TX (default on; off saves airtime for soak tests). */
esp_err_t dashcdg_badge_prefs_load_karaoke_v4_stats_tx(uint8_t *out_on);
esp_err_t dashcdg_badge_prefs_save_karaoke_v4_stats_tx(uint8_t on);
/** Media path policy: 0=both, 1=mcast_prefer, 2=ucast_prefer, 3=auto (default auto). */
esp_err_t dashcdg_badge_prefs_load_karaoke_media_path_policy(uint8_t *out_mode);
esp_err_t dashcdg_badge_prefs_save_karaoke_media_path_policy(uint8_t mode);
