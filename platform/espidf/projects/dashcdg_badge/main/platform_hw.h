#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Audio lab (Mary demo) PCM sample rate: must match `badge_lab_ym.c` esp_timer period.
 * 24 kHz matches half the desktop v4 default wire rate (48 kHz) for mental alignment.
 * ESP32 DAC default digital clock supports ~19.6 kHz–MHz (not 16 kHz).
 */
#ifndef DASHCDG_LAB_PCM_FS_HZ
#define DASHCDG_LAB_PCM_FS_HZ 24000u
#endif

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
 * Call once after `dashcdg_badge_rx_stop()` completes when multicast RX has torn down.
 * Applies deferred WiFi modem power-save restore when the UI left karaoke before RX stopped
 * (navigation sets screen → HOME while `badge_rx` was still running — must not restore PS until here).
 */
void dashcdg_platform_hw_on_badge_rx_stopped(void);

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
 * LVGL karaoke tick after CDG overlay blit (or ~33 ms cadence): keeps auto sleep from firing while
 * the UI is actively driving the CDG slot even if the jitter buffer briefly reads empty.
 */
void dashcdg_platform_hw_note_karaoke_cdg_overlay_tick(uint64_t now_ms);

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

/**
 * Lab stream on IO26 (exclusive with UI beep sequences).
 * On ESP32: same `dac_continuous` handle + amp path as karaoke (`DASHCDG_LAB_PCM_FS_HZ` nominal); Mary pushes s16.
 * Otherwise: LEDC fixed-carrier PWM duty stream.
 */
bool dashcdg_platform_hw_lab_pcm_stream_begin(void);
void dashcdg_platform_hw_lab_pcm_stream_end(void);
/** True while lab owns IO26 (UI beeps suppressed; `beep_seq_tick` idle). */
bool dashcdg_platform_hw_lab_pcm_is_streaming(void);
void dashcdg_platform_hw_lab_pcm_push_u8(uint8_t duty_u8);
uint8_t dashcdg_platform_hw_get_beep_volume_pct(void);

/**
 * Karaoke v4 decoded PCM -> ESP32 native DAC (mono, IO26). Call only from `badge_rx` task.
 * Do not call `begin` while the audio-lab Mary stream is active; the lab `stream_begin` path reuses this DAC and replaces RX.
 *
 * `nominal_hz` is the PCM sample rate from decode (e.g. 8000 AMR-NB, 16000 AMR-WB, 48000 Opus).
 * DAC continuous `freq_hz` is nominal adjusted by trim PPM (integer Hz; sub‑ppm drift needs drop/dup or SRC).
 */
bool dashcdg_platform_hw_karaoke_dac_begin_nominal_hz(uint32_t nominal_hz);
/** Back-compat: same as begin_nominal_hz(48000). */
bool dashcdg_platform_hw_karaoke_dac_begin(void);
void dashcdg_platform_hw_karaoke_dac_stop(void);
/**
 * Glitch-free session/pause/resume break: emit any partial chunk, then a short DC-silence run
 * so the DMA tails off cleanly without snapping to the next session's first sample. Keeps the
 * `dac_continuous` handle alive across track changes — the audible pop on session_info / pause
 * resume was caused by repeated `dac_continuous_disable` + `dac_continuous_del_channels`.
 */
void dashcdg_platform_hw_karaoke_dac_session_break(void);
/** Playback trim in ppm (−500000…500000 typical); applied when opening DAC (integer Hz quantum). */
void dashcdg_platform_hw_karaoke_dac_set_trim_ppm(int32_t ppm);
void dashcdg_platform_hw_karaoke_dac_push_mono_s16(const int16_t *pcm, size_t samples);
/** @deprecated Use dashcdg_platform_hw_karaoke_dac_push_mono_s16 (same behavior). */
void dashcdg_platform_hw_karaoke_dac_push_mono_s16_48k(const int16_t *pcm, size_t samples);
/** If a partial native-DAC DMA chunk is buffered, pad with mid-scale and flush. */
void dashcdg_platform_hw_karaoke_dac_pad_partial_chunk(void);
/**
 * UART proof counters (ESP32): successful `dac_continuous_write` chunk count vs failures.
 * Reset when karaoke DAC opens successfully. eff_hz_out/chunk_u8_out describe active DMA timing.
 * Optional `pending_pcm_u8_fill_out`: snapshot of partial-chunk fill (bytes in `karaoke_dac_push` queue
 * before the next full `dac_continuous_write`). Pass NULL if unused.
 */
void dashcdg_platform_hw_karaoke_dac_get_uart_health(
        uint32_t *dma_chunks_ok, uint32_t *dma_write_err, uint32_t *eff_hz_out, uint32_t *chunk_u8_out,
        uint32_t *pending_pcm_u8_fill_out);
/**
 * Release SC8002B shutdown (amp on) when karaoke RX starts so the speaker path is not left muted
 * until the first decoded frame (no startup click through DAC otherwise).
 */
void dashcdg_platform_hw_karaoke_amp_arm_for_rx(void);

/**
 * DAC route arbitration (T6 - executive refactor). The same `dac_continuous` channel and amp path
 * are shared between karaoke RX (decoded multicast PCM) and the audio lab (YM/Mary synth). Before
 * T6 the two producers raced on `s_karaoke_dac_io_mtx` with portMAX_DELAY waits and there was no
 * explicit owner -- a stale stream could chop the other's output.
 *
 * The route owner is a single enum (NONE / KARAOKE_RX / AUDIO_LAB). Producers MUST claim the route
 * before opening the DAC and MUST release it after stop. Claims are idempotent for the same owner.
 *
 * Claim returns:
 *   - true when the route is now held by `owner` (was NONE, or was already `owner`).
 *   - false when a different owner holds the route. Caller must back off.
 *
 * Release is a no-op when the caller is not the current owner (safe to call defensively).
 *
 * See docs/specs/esp32-badge-freertos-executive-refactor-spec.md §3.5 (HW owner / DAC route).
 */
typedef enum {
    DASHCDG_DAC_ROUTE_NONE = 0,
    DASHCDG_DAC_ROUTE_KARAOKE_RX = 1,
    DASHCDG_DAC_ROUTE_AUDIO_LAB = 2,
} dashcdg_dac_route_owner_t;

bool dashcdg_platform_hw_dac_route_claim(dashcdg_dac_route_owner_t owner);
void dashcdg_platform_hw_dac_route_release(dashcdg_dac_route_owner_t owner);
dashcdg_dac_route_owner_t dashcdg_platform_hw_dac_route_owner(void);

/**
 * Bounded-wait + arbitration counters for telemetry / soak triage. Pass NULL for any field
 * you don't care about.
 *   - wrong_owner_drops      : push_mono_s16 calls rejected because the wrong owner holds the route
 *   - io_mtx_timeouts_push   : s_karaoke_dac_io_mtx bounded-wait timeouts in the PCM push path
 *   - io_mtx_timeouts_stop   : s_karaoke_dac_io_mtx bounded-wait timeouts in the DAC stop path
 *   - route_claim_conflicts  : claim() calls that returned false because a different owner held the route
 */
void dashcdg_platform_hw_dac_route_get_stats(uint32_t *wrong_owner_drops,
                                             uint32_t *io_mtx_timeouts_push,
                                             uint32_t *io_mtx_timeouts_stop,
                                             uint32_t *route_claim_conflicts);

/** Karaoke RX line-out volume 0–100 (linear; 0 ⇒ mute path / amp off request). */
uint8_t dashcdg_platform_hw_get_karaoke_output_volume_pct(void);
void dashcdg_platform_hw_set_karaoke_output_volume_pct(uint8_t pct_0_100);

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
 * Thread-safe snapshot of the last battery sample taken inside the platform task
 * (adaptive cadence: active ~1 s, karaoke ~1.5 s, sleep ~4 s, plus IIR smoothing).
 * Falls back to immediate `dashcdg_vbat_sense_read` if the task is not running.
 */
esp_err_t dashcdg_platform_hw_battery_read(int *out_raw, int *out_pin_mv, int *out_vbat_mv);
