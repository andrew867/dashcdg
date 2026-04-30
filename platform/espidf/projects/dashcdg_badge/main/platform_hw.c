/*
 * Low-priority FreeRTOS task: RGB status LED (LEDC), backlight PWM (IO27), battery cache,
 * user button (IO0), SC8002B enable (IO4), audio on IO26: UI = LEDC tones; lab + karaoke v4 = ESP32 DAC
 * continuous (I2S0 DMA) when `CONFIG_IDF_TARGET_ESP32`, else LEDC PWM PCM. Beep paths must not touch IO26
 * while either DAC stream is active (LEDC vs DAC conflict; mute must not assert /SHDN during karaoke).
 */
#include "platform_hw.h"

#include "board_badge_hw.h"
#include "board_cyd_freenove_32.h"
#include "badge_prefs.h"
#include "badge_rx.h"

#include "dashcdg/media_clock.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#if CONFIG_IDF_TARGET_ESP32
#include "driver/dac_continuous.h"
#ifndef DASHCDG_HW_ESP32_DAC_LINE_CHANNEL_MASK
/*
 * ESP-IDF v5.5.x `dac_channel_mask_t` (see peripherals/dac.html): DAC_CHANNEL_MASK_CH0 = GPIO25,
 * DAC_CHANNEL_MASK_CH1 = GPIO26. Intro text calls those "channel 1" / "channel 2" by pin order —
 * IO26 is driver CH1, not "mask CH2" (there is no CH2 on ESP32).
 */
#define DASHCDG_HW_ESP32_DAC_LINE_CHANNEL_MASK DAC_CHANNEL_MASK_CH1
#endif
#endif
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "vbat_sense.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "platform_hw";

#define HW_TASK_STACK  3072
#define HW_TASK_PRIO   1
#define HW_TICK_MS     40
/* Battery ADC cadence: slow in sleep, moderate in active UI (cached for callers). */
#define BAT_SAMPLE_ACTIVE_MS 1000U
#define BAT_SAMPLE_KARAOKE_MS 1500U
#define BAT_SAMPLE_SLEEP_MS 4000U
/* IIR smoothing: new = old*(1-1/4) + sample*(1/4). */
#define BAT_EMA_SHIFT 2U
#define AMP_MIN_ON_DWELL_MS 180U
#define AMP_MIN_OFF_DWELL_MS 40U

#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_TIMER_RGB_BL   LEDC_TIMER_0
#define LEDC_TIMER_BEEP     LEDC_TIMER_1
#define LEDC_CH_R           LEDC_CHANNEL_0
#define LEDC_CH_G           LEDC_CHANNEL_1
#define LEDC_CH_B           LEDC_CHANNEL_2
#define LEDC_CH_BL          LEDC_CHANNEL_3
#define LEDC_CH_AUDIO       LEDC_CHANNEL_4

#define LEDC_DUTY_RES       LEDC_TIMER_8_BIT
#define LEDC_RGB_BL_HZ      5000
#define LEDC_BEEP_HZ        1760 /* initial install; runtime freq set per note */

#define BEEP_SEQ_CAP        12
#define BEEP_TICK_FAST_MS   15 /* while a sequence runs; slower tick = less zipper noise on PWM */

typedef enum {
    PM_ACTIVE = 0,
    PM_DIM,
    PM_SLEEP_FADE, /* backlight ramp to 0, panel still on */
    PM_SLEEP,
    PM_WAKE_FADE, /* panel on, backlight 0 -> user */
} pm_state_t;

static TaskHandle_t s_hw_task;
static SemaphoreHandle_t s_mtx;

static bool s_ready;
static dashcdg_hw_screen_t s_screen;
static bool s_cdg_stream_ok;
/** Last multicast UDP recv time (RX task); karaoke PM uses with `DASHCDG_HW_IDLE_DIM_MS`. */
static _Atomic uint64_t s_karaoke_last_mcast_rx_ms;
/** Last CDG overlay tick (LVGL); combined with RX so jitter-empty gaps do not arm idle sleep. */
static _Atomic uint64_t s_karaoke_last_overlay_ms;
/** Throttle `pm_bump_activity_locked` from high packet rates (ms, monotonic clock). */
static uint64_t s_karaoke_mcast_act_throttle_ms;

/** User-selected max brightness (NVS / settings). */
static uint8_t s_bl_user_pct = 100;
/** Current backlight applied (may be ramped down for idle dim / zero in sleep). */
static uint8_t s_bl_applied_pct = 100;

static uint64_t s_activity_ms;
static pm_state_t s_pm_state;
static volatile int32_t s_disp_pwrcmd;

static bool s_wifi_ps_saved_valid;
static wifi_ps_type_t s_wifi_ps_saved;
/** PS before karaoke streaming; restored when leaving karaoke (separate from panel-sleep PS save). */
static bool s_wifi_ps_karaoke_saved_valid;
static wifi_ps_type_t s_wifi_ps_karaoke_saved;

static bool s_rgb_status_enabled = true;
/** After hard-off, RGB pins are GPIO-driven high; re-bind LEDC before animating again. */
static bool s_rgb_ledc_detached;
static uint8_t s_rgb_status_pct = 100;
static bool s_auto_sleep_enabled = true;

/** Stashed when panel commits sleep; LVGL consumes on wake (see `dashcdg_platform_hw_consume_post_wake_ui_mask`). */
static volatile uint32_t s_post_wake_ui_mask;

static int s_bat_raw;
static int s_bat_pin_mv;
static int s_bat_vbat_mv;
static uint64_t s_bat_sample_ms;
static bool s_bat_cache_valid;

typedef struct {
    uint16_t freq_hz;
    uint16_t duration_ms;
} beep_note_t;

typedef enum {
    BEEP_SEQ_NONE = 0,
    BEEP_SEQ_WAKE,
    BEEP_SEQ_SLEEP,
    BEEP_SEQ_UI,
    BEEP_SEQ_BLIP,
} beep_seq_kind_t;

static beep_note_t s_seq[BEEP_SEQ_CAP];
static uint8_t s_seq_len;
static uint8_t s_seq_idx;
static uint64_t s_seq_note_t0_ms;
static uint64_t s_seq_note_t1_ms;
static beep_seq_kind_t s_seq_kind;

/** 5-100: scales peak LEDC duty on IO26 (default 85; was hardcoded ~50% peak). */
/** Written under `s_mtx`; read lock-free from lab PCM task (single-byte load on ESP32). */
static volatile uint8_t s_beep_vol_pct = 85;
/** LVGL UI tones (button triad, slider blip); power jingles ignore this. */
static bool s_touch_beep_on = true;
static bool s_amp_run_state;
static uint64_t s_amp_last_switch_ms;

static const beep_note_t k_wake_seq[] = {
    {784, 125},  /* G5 */
    {988, 125},  /* B5 */
    {1175, 140}, /* D6 */
    {1319, 330}, /* E6 "channel open" */
};

static const beep_note_t k_sleep_seq[] = {
    {1568, 155}, /* G6 */
    {1047, 185}, /* C6 */
    {698, 360},  /* F5 */
};

/** Slow "doo-dah-DEE" on buttons; mid-range only (high carriers + PWM = hash). */
static const beep_note_t k_ui_seq[] = {
    {784, 175},  /* G5 */
    {659, 210},  /* E5 */
    {1047, 300}, /* C6 */
};

static const beep_note_t k_blip_seq[] = {
    {784, 135}, /* G5 soft preview */
};

/*
 * AY-3-8910-class PSG + DMA PCM is very feasible on this chip: core emu is typically a few KB
 * of code plus ~1–4 KB ring buffer at 32–44.1 kHz mono before any waveform LUTs. Not integrated
 * here — output is still LEDC multi-tone; a future path could stream PCM via I2S or RMT DMA into
 * the same amp chain for recognizable square/wave chip timbres.
 */

static bool beep_queue_copy_locked(const beep_note_t *src, uint8_t n, beep_seq_kind_t kind, uint64_t now);

static uint8_t s_btn_low_streak;
/** After sleep or wake from IO0, ignore further button edges until GPIO0 is released (avoids sleep->wake loop while held). */
static bool s_btn_block_until_hi;

/** Throttle slider preview blips (VALUE_CHANGED can fire very fast). */
static uint64_t s_last_blip_queued_ms;
/** Throttle indev button triads so double-events / tight navigation do not stack sequences. */
static uint64_t s_last_ui_queued_ms;

/** When true, `beep_seq_tick` yields IO26 to `dashcdg_platform_hw_lab_pcm_push_u8`. */
static volatile bool s_lab_pcm_streaming;

#if CONFIG_IDF_TARGET_ESP32
/** When true, native DAC owns GPIO26; do not touch LEDC_CH_AUDIO or amp shutdown from beep paths. */
static volatile bool s_karaoke_pcm_streaming;
/** DAC DMA writes in fixed chunks (driver packs 8-bit samples to 16-bit I2S slots on ESP32). */
#define LAB_DAC_PCM_CHUNK 256u
static dac_continuous_handle_t s_dac_lab_handle;
static uint8_t s_dac_lab_chunk[LAB_DAC_PCM_CHUNK];
static size_t s_dac_lab_fill;
/** 1st-order low-pass state for light zipper / harmonic taming at 24 kHz. */
static uint8_t s_dac_lp_u8 = 128;
/*
 * Mono 8-bit DAC stream. ESP32 DAC DMA cannot always clock very low sample rates directly; we clamp
 * hardware `freq_hz` to a safe floor and upsample in software when needed.
 * DMA chunk in samples: 160 @ ≤12 kHz nominal (20 ms @ 8 k), else 320.
 */
#define KARAOKE_DAC_CHUNK_SAMPLES_MAX 512u
#define KARAOKE_DAC_MIN_SAFE_HZ 24000u
/** Linear PCM → 8-bit DAC sample gain numerator (denominator 32768); higher = louder line-out. */
#ifndef KARAOKE_DAC_PCM_GAIN_NUM
#define KARAOKE_DAC_PCM_GAIN_NUM 200
#endif
static dac_continuous_handle_t s_karaoke_dac_handle;
static uint8_t s_karaoke_dac_chunk[KARAOKE_DAC_CHUNK_SAMPLES_MAX];
static size_t s_karaoke_dac_fill;
static size_t s_karaoke_dac_chunk_samples = 320u;
static uint32_t s_karaoke_dac_open_nominal_hz;
static uint32_t s_karaoke_dac_open_effective_hz;
static int32_t s_karaoke_dac_trim_ppm;
static uint32_t s_karaoke_dac_upsample_accum;
/*
 * When DMA buffer alloc fails (NO_MEM), badge_rx was calling begin() on every push → hundreds of
 * failed allocs/sec, no headroom recovery, and amp/DAC "pop" as enable is retried forever.
 */
static TickType_t s_karaoke_dac_begin_cool_until_tick;

static uint32_t karaoke_dac_effective_hz(uint32_t nominal_hz, int32_t ppm)
{
    uint64_t n64 = (uint64_t)nominal_hz;
    int64_t ppm64 = (int64_t)ppm;
    uint64_t adj;

    if (nominal_hz == 0U) {
        return 48000U;
    }
    if (ppm64 > 500000LL) {
        ppm64 = 500000LL;
    } else if (ppm64 < -500000LL) {
        ppm64 = -500000LL;
    }
    adj = (n64 * (uint64_t)(1000000LL + ppm64)) / 1000000ULL;
    if (adj < (uint64_t)KARAOKE_DAC_MIN_SAFE_HZ) {
        adj = (uint64_t)KARAOKE_DAC_MIN_SAFE_HZ;
    }
    if (adj > 96000ULL) {
        adj = 96000ULL;
    }
    return (uint32_t)adj;
}

static size_t karaoke_dac_pick_chunk_samples(uint32_t nominal_hz)
{
    if (nominal_hz <= 12000U) {
        return 160U;
    }
    return 320U;
}

/*
 * Software upsampling (nom Hz PCM → eff Hz DAC) must emit an integer number of u8 samples per
 * nominal frame (e.g. 160×24000/8000 = 480). If DMA chunk (often 320 @24k) does not divide that
 * total, pad_partial inserts silence every frame → slow / broken playback.
 */
static size_t karaoke_dac_gcd_size(size_t a, size_t b)
{
    while (b != 0U) {
        size_t t = a % b;

        a = b;
        b = t;
    }
    return a;
}

static size_t karaoke_dac_pick_output_chunk_samples(uint32_t nominal_hz, uint32_t eff_hz)
{
    size_t base_eff_chunk = karaoke_dac_pick_chunk_samples(eff_hz);
    size_t nom_chunk;

    if (nominal_hz == 0U || eff_hz <= nominal_hz) {
        return base_eff_chunk;
    }
    nom_chunk = karaoke_dac_pick_chunk_samples(nominal_hz);
    {
        uint64_t p = (uint64_t)nom_chunk * (uint64_t)eff_hz;
        uint64_t outw;

        if ((p % (uint64_t)nominal_hz) != 0ULL) {
            return base_eff_chunk;
        }
        outw = p / (uint64_t)nominal_hz;
        if (outw == 0ULL) {
            return base_eff_chunk;
        }
        if (outw <= (uint64_t)KARAOKE_DAC_CHUNK_SAMPLES_MAX) {
            return (size_t)outw;
        }
        {
            size_t g = karaoke_dac_gcd_size((size_t)outw, base_eff_chunk);

            if (g > KARAOKE_DAC_CHUNK_SAMPLES_MAX) {
                g = KARAOKE_DAC_CHUNK_SAMPLES_MAX;
            }
            if (g < 64U) {
                return base_eff_chunk;
            }
            return g;
        }
    }
}
#endif

static int beep_kind_priority(beep_seq_kind_t k)
{
    switch (k) {
    case BEEP_SEQ_WAKE:
    case BEEP_SEQ_SLEEP:
        return 4;
    case BEEP_SEQ_UI:
        return 2;
    case BEEP_SEQ_BLIP:
        return 1;
    default:
        return 0;
    }
}

#define USER_BTN_WAKE_FRAMES   5U   /* ~200 ms @ 40 ms/tick */
#define USER_BTN_SLEEP_FRAMES  35U  /* ~1.4 s hold to sleep */

/** Min gap between queued slider preview blips (fast VALUE_CHANGED). */
#define BEEP_BLIP_MIN_GAP_MS   95U
/** Min gap between indev button triads (duplicate CLICKED / tight double-tap). */
#define BEEP_UI_MIN_GAP_MS     145U

/** Sleep fade from IO0 long-hold: faster ramp to black than idle auto-sleep. */
static bool s_bl_sleep_fade_manual;

static bool pm_idle_eligible_locked(uint64_t now)
{
    if (s_screen == DASHCDG_HW_SCREEN_HOME) {
        return true;
    }
    if (s_screen == DASHCDG_HW_SCREEN_KARAOKE) {
        if (s_cdg_stream_ok) {
            return false;
        }
        uint64_t last_rx = atomic_load_explicit(&s_karaoke_last_mcast_rx_ms, memory_order_relaxed);
        uint64_t last_ov = atomic_load_explicit(&s_karaoke_last_overlay_ms, memory_order_relaxed);
        uint64_t last_live = (last_rx > last_ov) ? last_rx : last_ov;
        if (last_live != 0ULL && now >= last_live && (now - last_live) < (uint64_t)DASHCDG_HW_IDLE_DIM_MS) {
            return false;
        }
        return true;
    }
    return false;
}

static bool pm_idle_eligible_effective_locked(uint64_t now)
{
    if (!s_auto_sleep_enabled) {
        return false;
    }
    return pm_idle_eligible_locked(now);
}

static void pm_force_active_restore_locked(void)
{
    if (s_pm_state == PM_SLEEP) {
        __atomic_store_n(&s_disp_pwrcmd, 2, __ATOMIC_SEQ_CST);
        if (s_wifi_ps_saved_valid) {
            (void)esp_wifi_set_ps(s_wifi_ps_saved);
            s_wifi_ps_saved_valid = false;
        }
    }
    s_bl_sleep_fade_manual = false;
    s_pm_state = PM_ACTIVE;
    s_bl_applied_pct = s_bl_user_pct;
}

static void pm_commit_panel_sleep_locked(void)
{
    uint32_t wake_mask = 0U;
    if (s_screen == DASHCDG_HW_SCREEN_SETTINGS || s_screen == DASHCDG_HW_SCREEN_APPLICATIONS || s_screen == DASHCDG_HW_SCREEN_DISPLAY ||
        s_screen == DASHCDG_HW_SCREEN_WIFI || s_screen == DASHCDG_HW_SCREEN_AUDIO_LAB) {
        wake_mask = 1U;
    } else if (s_screen == DASHCDG_HW_SCREEN_KARAOKE) {
        wake_mask = 2U;
    }
    __atomic_store_n(&s_post_wake_ui_mask, wake_mask, __ATOMIC_RELEASE);

    s_bl_sleep_fade_manual = false;
    __atomic_store_n(&s_disp_pwrcmd, 1, __ATOMIC_SEQ_CST);
    wifi_ps_type_t cur = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&cur) == ESP_OK) {
        s_wifi_ps_saved = cur;
        s_wifi_ps_saved_valid = true;
        (void)esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    }
    s_pm_state = PM_SLEEP;
    s_bl_applied_pct = 0;
    s_btn_block_until_hi = true;

    if (wake_mask == 2U) {
        /* Stop UDP + IGMP while panel is off; karaoke LVGL tree stays until user leaves or wake resumes RX. */
        dashcdg_badge_rx_stop();
    }
}

static void pm_request_manual_sleep_fade_locked(uint64_t now)
{
    if (s_pm_state == PM_SLEEP || s_pm_state == PM_SLEEP_FADE) {
        return;
    }
    s_bl_sleep_fade_manual = true;
    s_pm_state = PM_SLEEP_FADE;
    if (s_touch_beep_on) {
        (void)beep_queue_copy_locked(k_sleep_seq, (uint8_t)(sizeof(k_sleep_seq) / sizeof(k_sleep_seq[0])), BEEP_SEQ_SLEEP, now);
    }
}

static uint8_t pm_idle_dim_target_pct_locked(void)
{
    uint8_t dim_tgt = (uint8_t)((s_bl_user_pct * (uint32_t)DASHCDG_HW_IDLE_DIM_PCT_OF_MAX) / 100U);
    if (dim_tgt < (uint8_t)DASHCDG_HW_IDLE_DIM_MIN_PCT) {
        dim_tgt = (uint8_t)DASHCDG_HW_IDLE_DIM_MIN_PCT;
    }
    return dim_tgt;
}

static void pm_backlight_fade_step_locked(void)
{
    if (s_pm_state == PM_SLEEP_FADE) {
        uint8_t p = s_bl_applied_pct;
        if (p > 0U) {
            uint8_t step;
            if (s_bl_sleep_fade_manual) {
                /* Button sleep: brisk ramp from any level down to off. */
                step = (uint8_t)((p + 5U) / 6U);
            } else {
                /* Idle auto-sleep: slower above idle-dim floor, quicker last miles to black. */
                uint8_t dim_floor = pm_idle_dim_target_pct_locked();
                if (p > (uint8_t)(dim_floor + 2U)) {
                    step = (uint8_t)((p + 24U) / 25U);
                } else {
                    step = (uint8_t)((p + 3U) / 4U);
                }
            }
            if (step < 1U) {
                step = 1U;
            }
            if (p > step) {
                s_bl_applied_pct = (uint8_t)(p - step);
            } else {
                s_bl_applied_pct = 0;
            }
        }
        if (s_bl_applied_pct == 0U) {
            pm_commit_panel_sleep_locked();
        }
        return;
    }
    if (s_pm_state == PM_WAKE_FADE) {
        uint8_t tgt = s_bl_user_pct;
        uint8_t p = s_bl_applied_pct;
        if (p < tgt) {
            uint32_t delta = (uint32_t)tgt - (uint32_t)p;
            /* Slower than idle 100->dim: smooth "device waking" ramp. */
            uint8_t step = (uint8_t)((delta + 47U) / 48U);
            if (step < 1U) {
                step = 1U;
            }
            if ((uint32_t)p + (uint32_t)step > (uint32_t)tgt) {
                s_bl_applied_pct = tgt;
            } else {
                s_bl_applied_pct = (uint8_t)(p + step);
            }
        }
        if (s_bl_applied_pct >= tgt) {
            s_pm_state = PM_ACTIVE;
        }
    }
}

static void pm_bump_activity_locked(uint64_t now)
{
    s_activity_ms = now;
    if (s_pm_state == PM_SLEEP) {
        __atomic_store_n(&s_disp_pwrcmd, 2, __ATOMIC_SEQ_CST);
        s_pm_state = PM_WAKE_FADE;
        s_bl_applied_pct = 0;
        if (s_wifi_ps_saved_valid) {
            (void)esp_wifi_set_ps(s_wifi_ps_saved);
            s_wifi_ps_saved_valid = false;
        }
        s_btn_block_until_hi = true;
        if (s_touch_beep_on) {
            (void)beep_queue_copy_locked(k_wake_seq, (uint8_t)(sizeof(k_wake_seq) / sizeof(k_wake_seq[0])), BEEP_SEQ_WAKE, now);
        }
    } else if (s_pm_state == PM_SLEEP_FADE) {
        s_bl_sleep_fade_manual = false;
        s_pm_state = PM_ACTIVE;
        s_bl_applied_pct = s_bl_user_pct;
    } else if (s_pm_state == PM_DIM) {
        s_pm_state = PM_ACTIVE;
        s_bl_applied_pct = s_bl_user_pct;
    }
}

static void pm_update_idle_locked(uint64_t now)
{
    if (!pm_idle_eligible_effective_locked(now)) {
        if (s_pm_state == PM_SLEEP) {
            __atomic_store_n(&s_disp_pwrcmd, 2, __ATOMIC_SEQ_CST);
            if (s_wifi_ps_saved_valid) {
                (void)esp_wifi_set_ps(s_wifi_ps_saved);
                s_wifi_ps_saved_valid = false;
            }
        }
        s_bl_sleep_fade_manual = false;
        s_pm_state = PM_ACTIVE;
        s_bl_applied_pct = s_bl_user_pct;
        return;
    }

    uint64_t idle = (now > s_activity_ms) ? (now - s_activity_ms) : 0U;

    if (idle >= (uint64_t)DASHCDG_HW_IDLE_SLEEP_MS && s_pm_state != PM_SLEEP && s_pm_state != PM_SLEEP_FADE &&
        s_pm_state != PM_WAKE_FADE) {
        s_bl_sleep_fade_manual = false;
        s_pm_state = PM_SLEEP_FADE;
        if (s_touch_beep_on) {
            (void)beep_queue_copy_locked(k_sleep_seq, (uint8_t)(sizeof(k_sleep_seq) / sizeof(k_sleep_seq[0])), BEEP_SEQ_SLEEP, now);
        }
        return;
    }

    if (idle >= (uint64_t)DASHCDG_HW_IDLE_DIM_MS && s_pm_state == PM_ACTIVE) {
        s_pm_state = PM_DIM;
    }

    if (s_pm_state == PM_DIM) {
        uint8_t dim_tgt = pm_idle_dim_target_pct_locked();
        if (s_bl_applied_pct > dim_tgt + 1U) {
            uint32_t div = (uint32_t)DASHCDG_HW_IDLE_DIM_RAMP_DIV;
            if (div < 1U) {
                div = 1U;
            }
            uint8_t step = (uint8_t)((s_bl_applied_pct - dim_tgt) / div);
            if (step < 1U) {
                step = 1U;
            }
            s_bl_applied_pct -= step;
        }
    }
}

/** Reattach RGB cathodes to LEDC timer (after GPIO force-off). Does not touch backlight channel. */
static esp_err_t rgb_status_ledc_channels_rebind(void)
{
    const gpio_num_t pins[] = {DASHCDG_HW_GPIO_RGB_R, DASHCDG_HW_GPIO_RGB_G, DASHCDG_HW_GPIO_RGB_B};
    const ledc_channel_t chans[] = {LEDC_CH_R, LEDC_CH_G, LEDC_CH_B};
    esp_err_t err = ESP_OK;

    for (unsigned i = 0; i < 3; i++) {
        gpio_reset_pin(pins[i]);
        ledc_channel_config_t ch = {0};
        ch.gpio_num = pins[i];
        ch.speed_mode = LEDC_MODE;
        ch.channel = chans[i];
        ch.intr_type = LEDC_INTR_DISABLE;
        ch.timer_sel = LEDC_TIMER_RGB_BL;
        ch.duty = 255;
        ch.hpoint = 0;
        err = ledc_channel_config(&ch);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "rgb ledc rebind: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

/**
 * Active-low cathodes to 3V3 through resistors: PWM "full duty off" still has tiny low phases → glow.
 * Stop LEDC with idle high, then drive GPIO outputs solid high so no sink current.
 */
static void rgb_pins_force_off_active_low(void)
{
    (void)ledc_stop(LEDC_MODE, LEDC_CH_R, 1);
    (void)ledc_stop(LEDC_MODE, LEDC_CH_G, 1);
    (void)ledc_stop(LEDC_MODE, LEDC_CH_B, 1);

    gpio_reset_pin(DASHCDG_HW_GPIO_RGB_R);
    gpio_reset_pin(DASHCDG_HW_GPIO_RGB_G);
    gpio_reset_pin(DASHCDG_HW_GPIO_RGB_B);

    gpio_config_t io = {
            .pin_bit_mask =
                    (1ULL << DASHCDG_HW_GPIO_RGB_R) | (1ULL << DASHCDG_HW_GPIO_RGB_G) | (1ULL << DASHCDG_HW_GPIO_RGB_B),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&io);
    gpio_set_level(DASHCDG_HW_GPIO_RGB_R, 1);
    gpio_set_level(DASHCDG_HW_GPIO_RGB_G, 1);
    gpio_set_level(DASHCDG_HW_GPIO_RGB_B, 1);
    s_rgb_ledc_detached = true;
}

static void rgb_ensure_ledc_attached(void)
{
    if (!s_rgb_ledc_detached) {
        return;
    }
    (void)rgb_status_ledc_channels_rebind();
    s_rgb_ledc_detached = false;
}

static void rgb_set_u8(uint8_t r, uint8_t g, uint8_t b)
{
    rgb_ensure_ledc_attached();
    /* GPIOs are active-low: higher LEDC duty => more time high => dimmer LED. */
    const uint32_t max_d = ((1U << 8) - 1U);
    uint32_t dr = max_d - (uint32_t)r;
    uint32_t dg = max_d - (uint32_t)g;
    uint32_t db = max_d - (uint32_t)b;
    if (dr > max_d) {
        dr = max_d;
    }
    if (dg > max_d) {
        dg = max_d;
    }
    if (db > max_d) {
        db = max_d;
    }
    ledc_set_duty(LEDC_MODE, LEDC_CH_R, dr);
    ledc_set_duty(LEDC_MODE, LEDC_CH_G, dg);
    ledc_set_duty(LEDC_MODE, LEDC_CH_B, db);
    ledc_update_duty(LEDC_MODE, LEDC_CH_R);
    ledc_update_duty(LEDC_MODE, LEDC_CH_G);
    ledc_update_duty(LEDC_MODE, LEDC_CH_B);
}

static void rgb_apply_status(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_rgb_status_enabled) {
        return;
    }
    uint32_t sc = (uint32_t)s_rgb_status_pct;
    if (sc > 100U) {
        sc = 100U;
    }
    r = (uint8_t)(((uint32_t)r * sc) / 100U);
    g = (uint8_t)(((uint32_t)g * sc) / 100U);
    b = (uint8_t)(((uint32_t)b * sc) / 100U);
    rgb_set_u8(r, g, b);
}

static void bl_set_pct(uint8_t pct)
{
    if (pct > 100U) {
        pct = 100U;
    }
    uint32_t duty = (uint32_t)pct * 255U / 100U;
    ledc_set_duty(LEDC_MODE, LEDC_CH_BL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CH_BL);
}

static void amp_set_run(bool run)
{
    uint64_t now_ms;

    if (run == s_amp_run_state) {
        return;
    }
    now_ms = dashcdg_clock_now_ms();
    if (!run && s_amp_run_state) {
        if (now_ms > s_amp_last_switch_ms &&
            (now_ms - s_amp_last_switch_ms) < (uint64_t)AMP_MIN_ON_DWELL_MS) {
            return;
        }
    } else if (run && !s_amp_run_state) {
        if (now_ms > s_amp_last_switch_ms &&
            (now_ms - s_amp_last_switch_ms) < (uint64_t)AMP_MIN_OFF_DWELL_MS) {
            return;
        }
    }
    /* SC8002B: VDD on shutdown = shutdown. LOW = amp on. */
    gpio_set_level(DASHCDG_HW_GPIO_AMP_SHUTDOWN, run ? 0 : 1);
    s_amp_run_state = run;
    s_amp_last_switch_ms = now_ms;
}

/** PWM to 0 only; does not touch amp shutdown (use between envelope samples while a jingle runs). */
static void beep_pwm_zero_locked(void)
{
#if CONFIG_IDF_TARGET_ESP32
    if (__atomic_load_n(&s_karaoke_pcm_streaming, __ATOMIC_RELAXED)) {
        return;
    }
#endif
    if (__atomic_load_n(&s_lab_pcm_streaming, __ATOMIC_RELAXED)) {
        return;
    }
    ledc_set_duty(LEDC_MODE, LEDC_CH_AUDIO, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CH_AUDIO);
}

/** Full mute: zero PWM and assert SC8002B shutdown (call when idle / sequence finished). */
static void beep_mute_locked(void)
{
#if CONFIG_IDF_TARGET_ESP32
    if (__atomic_load_n(&s_karaoke_pcm_streaming, __ATOMIC_RELAXED)) {
        return;
    }
#endif
    if (__atomic_load_n(&s_lab_pcm_streaming, __ATOMIC_RELAXED)) {
        return;
    }
    beep_pwm_zero_locked();
    amp_set_run(false);
}

/**
 * Map saved 5..100 "percent" to LEDC duty with a strong low-end taper.
 * Linear mapping made 7–8% painfully loud through the SC8002B + divider; use a power law
 * so low settings are actually quiet (perceptual-ish without full log10 on every tick).
 */
static uint32_t beep_peak_duty_u8(void)
{
    float p = (float)s_beep_vol_pct;
    if (p < 5.f) {
        p = 5.f;
    }
    if (p > 100.f) {
        p = 100.f;
    }
    float t = (p - 5.f) / 95.f; /* 0 at slider min, 1 at max */
    /* ~t^3.4: at 8% UI, duty stays in low single digits; near max still reaches full scale */
    float g = powf(t, 3.4f);
    uint32_t d = (uint32_t)(g * 255.f + 0.5f);
    if (d < 1U && p >= 6.f) {
        d = 1U;
    }
    if (d > 255U) {
        d = 255U;
    }
    return d;
}

/** Half-sine bell 0..1..0 over note length => soft attack/decay on square PWM ("AM sine"). */
static float beep_envelope(uint32_t elapsed_ms, uint32_t dur_ms)
{
    if (dur_ms < 1U) {
        return 0.f;
    }
    double ph = (double)elapsed_ms / (double)dur_ms;
    if (ph < 0.0) {
        ph = 0.0;
    }
    if (ph > 1.0) {
        ph = 1.0;
    }
    return (float)sin(M_PI * ph);
}

static void beep_apply_freq_duty_locked(uint32_t freq_hz, uint32_t duty_u8)
{
#if CONFIG_IDF_TARGET_ESP32
    if (__atomic_load_n(&s_karaoke_pcm_streaming, __ATOMIC_RELAXED)) {
        return;
    }
#endif
    if (__atomic_load_n(&s_lab_pcm_streaming, __ATOMIC_RELAXED)) {
        return;
    }
    if (freq_hz < 400U) {
        freq_hz = 400U;
    }
    if (freq_hz > 8000U) {
        freq_hz = 8000U;
    }
    /* Do not assert amp shutdown when duty is ~0: envelope start/end and gaps between notes
     * would toggle SC8002B every few ms and kill level / add thumps. Keep amp on; silence = PWM 0. */
    amp_set_run(true);
    if (duty_u8 < 1U) {
        ledc_set_freq(LEDC_MODE, LEDC_TIMER_BEEP, freq_hz);
        beep_pwm_zero_locked();
        return;
    }
    if (duty_u8 > 255U) {
        duty_u8 = 255U;
    }
    ledc_set_freq(LEDC_MODE, LEDC_TIMER_BEEP, freq_hz);
    ledc_set_duty(LEDC_MODE, LEDC_CH_AUDIO, duty_u8);
    ledc_update_duty(LEDC_MODE, LEDC_CH_AUDIO);
}

static bool beep_queue_copy_locked(const beep_note_t *src, uint8_t n, beep_seq_kind_t kind, uint64_t now)
{
    int neu;
    int cur;

    if (src == NULL || n == 0U || n > BEEP_SEQ_CAP) {
        return false;
    }
    /* Lab / karaoke own IO26; never queue UI/blip on top of DAC PCM. */
    if (kind == BEEP_SEQ_UI || kind == BEEP_SEQ_BLIP) {
        if (__atomic_load_n(&s_lab_pcm_streaming, __ATOMIC_RELAXED)) {
            return false;
        }
#if CONFIG_IDF_TARGET_ESP32
        if (__atomic_load_n(&s_karaoke_pcm_streaming, __ATOMIC_RELAXED)) {
            return false;
        }
#endif
    }
    neu = beep_kind_priority(kind);
    cur = beep_kind_priority(s_seq_kind);
    if (s_seq_len > 0U && s_seq_idx < s_seq_len && neu < cur) {
        return false;
    }
    memcpy(s_seq, src, (size_t)n * sizeof(beep_note_t));
    s_seq_len = n;
    s_seq_kind = kind;
    s_seq_idx = 0;
    s_seq_note_t0_ms = now;
    s_seq_note_t1_ms = now + (uint64_t)s_seq[0].duration_ms;
    return true;
}

static void beep_seq_tick_locked(uint64_t now)
{
    if (__atomic_load_n(&s_lab_pcm_streaming, __ATOMIC_RELAXED)) {
        return;
    }
#if CONFIG_IDF_TARGET_ESP32
    if (__atomic_load_n(&s_karaoke_pcm_streaming, __ATOMIC_RELAXED)) {
        return;
    }
#endif
    /* Idle: do NOT call `beep_mute_locked` every tick — that was toggling /SHDN + PWM at 15–40 Hz and
     * choked UI beeps (amp never stayed on long enough for the triad envelope). */
    if (s_seq_len == 0U) {
        return;
    }
    if (s_seq_idx >= s_seq_len) {
        s_seq_len = 0U;
        s_seq_kind = BEEP_SEQ_NONE;
        beep_mute_locked();
        return;
    }

    while (s_seq_idx < s_seq_len && now >= s_seq_note_t1_ms) {
        s_seq_idx++;
        if (s_seq_idx >= s_seq_len) {
            s_seq_len = 0U;
            s_seq_kind = BEEP_SEQ_NONE;
            beep_mute_locked();
            return;
        }
        s_seq_note_t0_ms = now;
        s_seq_note_t1_ms = now + (uint64_t)s_seq[s_seq_idx].duration_ms;
    }

    const beep_note_t *n = &s_seq[s_seq_idx];
    uint32_t elapsed = (uint32_t)(now - s_seq_note_t0_ms);
    uint32_t dur = (uint32_t)n->duration_ms;
    if (dur < 1U) {
        dur = 1U;
    }
    float env = beep_envelope(elapsed, dur);
    uint32_t peak = beep_peak_duty_u8();
    uint32_t duty = (uint32_t)((float)peak * env);
    if (duty < 1U && env > 0.08f) {
        duty = 1U;
    }

    beep_apply_freq_duty_locked((uint32_t)n->freq_hz, duty);
}

static bool beep_seq_active(void)
{
    return s_seq_len > 0U && s_seq_idx < s_seq_len;
}

/** Drop UI/wake/sleep sequence state (does not touch GPIO; caller often follows with mute or lab setup). */
static void beep_seq_abort_locked(void)
{
    s_seq_len = 0U;
    s_seq_idx = 0U;
    s_seq_kind = BEEP_SEQ_NONE;
}

static esp_err_t ledc_install_rgb_bl(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER_RGB_BL,
        .freq_hz = LEDC_RGB_BL_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc_timer RGB/BL");

    const gpio_num_t pins[] = {DASHCDG_HW_GPIO_RGB_R, DASHCDG_HW_GPIO_RGB_G, DASHCDG_HW_GPIO_RGB_B, DASHCDG_HW_GPIO_LCD_BL_PWM};
    const ledc_channel_t chans[] = {LEDC_CH_R, LEDC_CH_G, LEDC_CH_B, LEDC_CH_BL};
    for (unsigned i = 0; i < 4; i++) {
        ledc_channel_config_t ch = {0};
        ch.gpio_num = pins[i];
        ch.speed_mode = LEDC_MODE;
        ch.channel = chans[i];
        ch.intr_type = LEDC_INTR_DISABLE;
        ch.timer_sel = LEDC_TIMER_RGB_BL;
        ch.duty = (i < 3) ? 255 : 0;
        ch.hpoint = 0;
        ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc_channel");
    }
    return ESP_OK;
}

static esp_err_t ledc_install_beep(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER_BEEP,
        .freq_hz = LEDC_BEEP_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc_timer beep");

    ledc_channel_config_t ch = {0};
    ch.gpio_num = DASHCDG_HW_GPIO_AUDIO_PWM;
    ch.speed_mode = LEDC_MODE;
    ch.channel = LEDC_CH_AUDIO;
    ch.intr_type = LEDC_INTR_DISABLE;
    ch.timer_sel = LEDC_TIMER_BEEP;
    ch.duty = 0;
    ch.hpoint = 0;
    return ledc_channel_config(&ch);
}

#if CONFIG_IDF_TARGET_ESP32
/** Re-bind IO26 to LEDC after DAC continuous teardown (UI beeps / mute). Caller holds `s_mtx`. */
static esp_err_t ledc_beep_audio_channel_attach_locked(void)
{
    ledc_channel_config_t ch = {0};
    if (s_karaoke_dac_handle != NULL || s_dac_lab_handle != NULL ||
        __atomic_load_n(&s_karaoke_pcm_streaming, __ATOMIC_RELAXED) ||
        __atomic_load_n(&s_lab_pcm_streaming, __ATOMIC_RELAXED)) {
        /* GPIO26 currently owned by DAC path; defer LEDC rebind. */
        return ESP_OK;
    }
    gpio_reset_pin(DASHCDG_HW_GPIO_AUDIO_PWM);
    ch.gpio_num = DASHCDG_HW_GPIO_AUDIO_PWM;
    ch.speed_mode = LEDC_MODE;
    ch.channel = LEDC_CH_AUDIO;
    ch.intr_type = LEDC_INTR_DISABLE;
    ch.timer_sel = LEDC_TIMER_BEEP;
    ch.duty = 0;
    ch.hpoint = 0;
    return ledc_channel_config(&ch);
}

/** Pad partial chunk with mid-scale silence, drain DMA, delete DAC handle, restore LEDC on IO26. */
static void lab_dac_flush_stop_and_ledc_restore_locked(void)
{
    if (s_dac_lab_handle == NULL) {
        return;
    }
    s_dac_lp_u8 = 128U;
    while (s_dac_lab_fill > 0U) {
        while (s_dac_lab_fill < LAB_DAC_PCM_CHUNK) {
            s_dac_lab_chunk[s_dac_lab_fill++] = 128U;
        }
        (void)dac_continuous_write(s_dac_lab_handle, s_dac_lab_chunk, LAB_DAC_PCM_CHUNK, NULL, -1);
        s_dac_lab_fill = 0U;
    }
    (void)dac_continuous_disable(s_dac_lab_handle);
    (void)dac_continuous_del_channels(s_dac_lab_handle);
    s_dac_lab_handle = NULL;
    (void)ledc_beep_audio_channel_attach_locked();
}

static void karaoke_dac_flush_stop_and_ledc_restore_locked(void)
{
    size_t chunk = s_karaoke_dac_chunk_samples;

    if (s_karaoke_dac_handle == NULL) {
        return;
    }
    if (chunk == 0U || chunk > KARAOKE_DAC_CHUNK_SAMPLES_MAX) {
        chunk = 320U;
    }
    while (s_karaoke_dac_fill > 0U) {
        while (s_karaoke_dac_fill < chunk) {
            s_karaoke_dac_chunk[s_karaoke_dac_fill++] = 128U;
        }
        (void)dac_continuous_write(s_karaoke_dac_handle, s_karaoke_dac_chunk, chunk, NULL, -1);
        s_karaoke_dac_fill = 0U;
    }
    (void)dac_continuous_disable(s_karaoke_dac_handle);
    (void)dac_continuous_del_channels(s_karaoke_dac_handle);
    s_karaoke_dac_handle = NULL;
    s_karaoke_dac_open_nominal_hz = 0U;
    s_karaoke_dac_open_effective_hz = 0U;
    s_karaoke_dac_upsample_accum = 0U;
    /*
     * Never rebind LEDC here. Stop/start churn around karaoke transitions can race ownership and
     * emit "GPIO 26 not usable". Reattach is centralized in screen-transition logic when UI leaves
     * karaoke mode.
     */
    __atomic_store_n(&s_karaoke_pcm_streaming, false, __ATOMIC_RELEASE);
}

void dashcdg_platform_hw_karaoke_amp_arm_for_rx(void)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(80)) != pdTRUE) {
        return;
    }
    amp_set_run(true);
    xSemaphoreGive(s_mtx);
}

bool dashcdg_platform_hw_karaoke_dac_begin_nominal_hz(uint32_t nominal_hz)
{
    esp_err_t de;
    uint32_t eff_hz;
    size_t chunk;

    if (!s_mtx) {
        return false;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(120)) != pdTRUE) {
        return false;
    }
    {
        TickType_t now = xTaskGetTickCount();
        if (s_karaoke_dac_begin_cool_until_tick != (TickType_t)0 && now < s_karaoke_dac_begin_cool_until_tick) {
            xSemaphoreGive(s_mtx);
            return false;
        }
    }
    if (nominal_hz == 0U) {
        nominal_hz = 48000U;
    }
    eff_hz = karaoke_dac_effective_hz(nominal_hz, s_karaoke_dac_trim_ppm);
    chunk = karaoke_dac_pick_output_chunk_samples(nominal_hz, eff_hz);
    if (chunk > KARAOKE_DAC_CHUNK_SAMPLES_MAX) {
        chunk = KARAOKE_DAC_CHUNK_SAMPLES_MAX;
    }
    if (s_dac_lab_handle != NULL) {
        /*
         * Audio-lab can leave a stale DAC handle behind if stream_end fails to take s_mtx during a
         * busy period. Recover ownership here so karaoke RX does not stay permanently silent.
         */
        if (!__atomic_load_n(&s_lab_pcm_streaming, __ATOMIC_RELAXED)) {
            lab_dac_flush_stop_and_ledc_restore_locked();
        } else {
            xSemaphoreGive(s_mtx);
            return false;
        }
    }
    if (s_karaoke_dac_handle != NULL && s_karaoke_dac_open_nominal_hz == nominal_hz &&
            s_karaoke_dac_open_effective_hz == eff_hz && s_karaoke_dac_chunk_samples == chunk) {
        xSemaphoreGive(s_mtx);
        return true;
    }
    if (s_karaoke_dac_handle != NULL) {
        karaoke_dac_flush_stop_and_ledc_restore_locked();
    }
    s_karaoke_dac_chunk_samples = chunk;
    beep_seq_abort_locked();
    (void)ledc_stop(LEDC_MODE, LEDC_CH_AUDIO, 0);
    {
        dac_continuous_config_t dcfg = {
            .chan_mask = DASHCDG_HW_ESP32_DAC_LINE_CHANNEL_MASK,
            /* Fewer descriptors → smaller DMA reservation; slight underrun risk if CPU stalls. */
            .desc_num = 4,
            .buf_size = (uint32_t)chunk,
            .freq_hz = eff_hz,
            .offset = 0,
            .clk_src = DAC_DIGI_CLK_SRC_DEFAULT,
            .chan_mode = DAC_CHANNEL_MODE_SIMUL,
        };
        de = dac_continuous_new_channels(&dcfg, &s_karaoke_dac_handle);
        if (de != ESP_OK) {
            /*
             * Log at most ~once per cooldown window; RX calls begin() every decode attempt otherwise.
             */
            static TickType_t s_karaoke_dac_err_log_suppress_until;
            TickType_t nowt = xTaskGetTickCount();
            if (nowt >= s_karaoke_dac_err_log_suppress_until) {
                ESP_LOGE(TAG,
                         "karaoke dac_continuous_new_channels: %s nom=%u eff=%u chunk=%u (internal_free=%u dma_free=%u largest_dma=%u)",
                         esp_err_to_name(de), (unsigned)nominal_hz, (unsigned)eff_hz, (unsigned)chunk,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
                s_karaoke_dac_err_log_suppress_until = nowt + pdMS_TO_TICKS(4000);
            }
            s_karaoke_dac_handle = NULL;
            if (de == ESP_ERR_NO_MEM) {
                s_karaoke_dac_begin_cool_until_tick = nowt + pdMS_TO_TICKS(2500);
            }
            (void)ledc_beep_audio_channel_attach_locked();
            xSemaphoreGive(s_mtx);
            return false;
        }
        de = dac_continuous_enable(s_karaoke_dac_handle);
        if (de != ESP_OK) {
            ESP_LOGE(TAG, "karaoke dac_continuous_enable: %s", esp_err_to_name(de));
            (void)dac_continuous_disable(s_karaoke_dac_handle);
            (void)dac_continuous_del_channels(s_karaoke_dac_handle);
            s_karaoke_dac_handle = NULL;
            (void)ledc_beep_audio_channel_attach_locked();
            xSemaphoreGive(s_mtx);
            return false;
        }
    }
    s_karaoke_dac_open_nominal_hz = nominal_hz;
    s_karaoke_dac_open_effective_hz = eff_hz;
    s_karaoke_dac_upsample_accum = 0U;
    s_karaoke_dac_begin_cool_until_tick = (TickType_t)0;
    s_karaoke_dac_fill = 0U;
    amp_set_run(true);
    __atomic_store_n(&s_karaoke_pcm_streaming, true, __ATOMIC_RELEASE);
    xSemaphoreGive(s_mtx);
    return true;
}

bool dashcdg_platform_hw_karaoke_dac_begin(void)
{
    return dashcdg_platform_hw_karaoke_dac_begin_nominal_hz(48000U);
}

void dashcdg_platform_hw_karaoke_dac_set_trim_ppm(int32_t ppm)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(80)) != pdTRUE) {
        return;
    }
    /*
     * Trim takes effect on the next `karaoke_dac_begin_nominal_hz`: effective Hz changes vs
     * `s_karaoke_dac_open_effective_hz`, so the DAC is torn down and recreated (typically next RX push).
     */
    s_karaoke_dac_trim_ppm = ppm;
    xSemaphoreGive(s_mtx);
}

void dashcdg_platform_hw_karaoke_dac_stop(void)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(120)) != pdTRUE) {
        return;
    }
    karaoke_dac_flush_stop_and_ledc_restore_locked();
    /* Next karaoke session should retry DAC alloc immediately (heap may have recovered). */
    s_karaoke_dac_begin_cool_until_tick = (TickType_t)0;
    s_karaoke_dac_trim_ppm = 0;
    beep_mute_locked();
    xSemaphoreGive(s_mtx);
}

void dashcdg_platform_hw_karaoke_dac_push_mono_s16(const int16_t *pcm, size_t samples)
{
    size_t chunk = s_karaoke_dac_chunk_samples;
    uint32_t nom_hz = s_karaoke_dac_open_nominal_hz;
    uint32_t eff_hz = s_karaoke_dac_open_effective_hz;

    if (pcm == NULL || samples == 0U || s_karaoke_dac_handle == NULL) {
        return;
    }
    if (chunk == 0U || chunk > KARAOKE_DAC_CHUNK_SAMPLES_MAX) {
        chunk = 320U;
    }
    amp_set_run(true);
    for (size_t i = 0U; i < samples; ++i) {
        int32_t s = (int32_t)pcm[i];
        int32_t u = (s * (int32_t)KARAOKE_DAC_PCM_GAIN_NUM) / 32768 + 128;
        uint32_t emit = 1U;

        if (u < 16) {
            u = 16;
        }
        if (u > 239) {
            u = 239;
        }
        if (nom_hz != 0U && eff_hz > nom_hz) {
            uint64_t acc = (uint64_t)s_karaoke_dac_upsample_accum + (uint64_t)eff_hz;
            emit = (uint32_t)(acc / (uint64_t)nom_hz);
            s_karaoke_dac_upsample_accum = (uint32_t)(acc % (uint64_t)nom_hz);
            if (emit == 0U) {
                emit = 1U;
            }
        }
        for (uint32_t r = 0U; r < emit; ++r) {
            s_karaoke_dac_chunk[s_karaoke_dac_fill++] = (uint8_t)u;
            if (s_karaoke_dac_fill >= chunk) {
                (void)dac_continuous_write(s_karaoke_dac_handle, s_karaoke_dac_chunk, chunk, NULL, -1);
                s_karaoke_dac_fill = 0U;
            }
        }
    }
}

void dashcdg_platform_hw_karaoke_dac_push_mono_s16_48k(const int16_t *pcm, size_t samples)
{
    dashcdg_platform_hw_karaoke_dac_push_mono_s16(pcm, samples);
}

void dashcdg_platform_hw_karaoke_dac_pad_partial_chunk(void)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(8)) != pdTRUE) {
        return;
    }
    if (s_karaoke_dac_handle != NULL && s_karaoke_dac_fill > 0U) {
        size_t chunk = s_karaoke_dac_chunk_samples;

        if (chunk == 0U || chunk > KARAOKE_DAC_CHUNK_SAMPLES_MAX) {
            chunk = 320U;
        }
        while (s_karaoke_dac_fill < chunk) {
            s_karaoke_dac_chunk[s_karaoke_dac_fill++] = 128U;
        }
        (void)dac_continuous_write(s_karaoke_dac_handle, s_karaoke_dac_chunk, chunk, NULL, -1);
        s_karaoke_dac_fill = 0U;
    }
    xSemaphoreGive(s_mtx);
}
#endif

#if !CONFIG_IDF_TARGET_ESP32
void dashcdg_platform_hw_karaoke_amp_arm_for_rx(void) {}

bool dashcdg_platform_hw_karaoke_dac_begin_nominal_hz(uint32_t nominal_hz)
{
    (void)nominal_hz;
    return false;
}

bool dashcdg_platform_hw_karaoke_dac_begin(void)
{
    return false;
}

void dashcdg_platform_hw_karaoke_dac_stop(void) {}

void dashcdg_platform_hw_karaoke_dac_set_trim_ppm(int32_t ppm)
{
    (void)ppm;
}

void dashcdg_platform_hw_karaoke_dac_push_mono_s16(const int16_t *pcm, size_t samples)
{
    (void)pcm;
    (void)samples;
}

void dashcdg_platform_hw_karaoke_dac_push_mono_s16_48k(const int16_t *pcm, size_t samples)
{
    (void)pcm;
    (void)samples;
}

void dashcdg_platform_hw_karaoke_dac_pad_partial_chunk(void) {}
#endif

static esp_err_t gpio_misc_init(void)
{
    gpio_config_t amp = {
        .pin_bit_mask = 1ULL << DASHCDG_HW_GPIO_AMP_SHUTDOWN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&amp), TAG, "gpio amp");
    /* Boot muted: shutdown asserted (HIGH). External 10k may already bias this; drive explicitly. */
    gpio_set_level(DASHCDG_HW_GPIO_AMP_SHUTDOWN, 1);
    s_amp_run_state = false;
    s_amp_last_switch_ms = 0U;

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << DASHCDG_HW_GPIO_USER_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ONLY,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&btn), TAG, "gpio btn");
    return ESP_OK;
}

static esp_err_t i2c_bus_init(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = DASHCDG_HW_GPIO_I2C_SDA,
        .scl_io_num = DASHCDG_HW_GPIO_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = DASHCDG_HW_I2C_FREQ_HZ,
    };
    esp_err_t e = i2c_param_config(DASHCDG_HW_I2C_PORT, &cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "i2c_param_config: %s", esp_err_to_name(e));
        return e;
    }
    e = i2c_driver_install(DASHCDG_HW_I2C_PORT, cfg.mode, 0, 0, 0);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "i2c_driver_install: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGI(TAG, "I2C%u ready (SDA IO%u SCL IO%u)", (unsigned)DASHCDG_HW_I2C_PORT, (unsigned)DASHCDG_HW_GPIO_I2C_SDA,
             (unsigned)DASHCDG_HW_GPIO_I2C_SCL);
    return ESP_OK;
}

static void sample_battery_update_cache(void)
{
    if (!dashcdg_vbat_sense_is_ready()) {
        return;
    }
    int raw = 0;
    int pin = 0;
    int vbat = 0;
    if (dashcdg_vbat_sense_read(&raw, &pin, &vbat) != ESP_OK) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (!s_bat_cache_valid) {
            s_bat_raw = raw;
            s_bat_pin_mv = pin;
            s_bat_vbat_mv = vbat;
            s_bat_cache_valid = true;
        } else {
            const int k = (1 << BAT_EMA_SHIFT);

            s_bat_raw = (s_bat_raw * (k - 1) + raw + (k / 2)) / k;
            s_bat_pin_mv = (s_bat_pin_mv * (k - 1) + pin + (k / 2)) / k;
            s_bat_vbat_mv = (s_bat_vbat_mv * (k - 1) + vbat + (k / 2)) / k;
        }
        s_bat_sample_ms = dashcdg_clock_now_ms();
        xSemaphoreGive(s_mtx);
    }
}

static uint32_t battery_sample_period_ms(void)
{
    if (s_pm_state == PM_SLEEP || s_pm_state == PM_SLEEP_FADE) {
        return BAT_SAMPLE_SLEEP_MS;
    }
    if (s_screen == DASHCDG_HW_SCREEN_KARAOKE) {
        return BAT_SAMPLE_KARAOKE_MS;
    }
    return BAT_SAMPLE_ACTIVE_MS;
}

static void led_anim_frame(uint64_t now_ms)
{
    if (!s_rgb_status_enabled) {
        return;
    }
    if (s_pm_state == PM_SLEEP) {
        /* Deep standby: very dim slow blue "breath" so the badge still feels alive. */
        float t = (float)(now_ms % 7000ULL) * (2.0f * (float)M_PI / 7000.0f);
        float breathe = 0.35f + 0.25f * (0.5f + 0.5f * sinf(t));
        uint8_t v = (uint8_t)(14.0f * breathe);
        rgb_apply_status(0, (uint8_t)(v / 2U), v);
        return;
    }

    float t = (float)(now_ms % 5000ULL) * (2.0f * (float)M_PI / 5000.0f);
    float breathe = 0.45f + 0.55f * (0.5f + 0.5f * sinf(t));

    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    switch (s_screen) {
    case DASHCDG_HW_SCREEN_HOME: {
        wifi_ap_record_t ap;
        bool assoc = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
        if (assoc) {
            /* Soft heartbeat: cool green/cyan when associated. */
            r = (uint8_t)(15 * breathe);
            g = (uint8_t)(160 * breathe);
            b = (uint8_t)(110 * breathe);
        } else {
            /* Idle / no AP: warm amber "breathing". */
            r = (uint8_t)(200 * breathe);
            g = (uint8_t)(90 * breathe);
            b = (uint8_t)(18 * breathe);
        }
        break;
    }
    case DASHCDG_HW_SCREEN_WIFI:
    case DASHCDG_HW_SCREEN_SETTINGS:
    case DASHCDG_HW_SCREEN_APPLICATIONS:
    case DASHCDG_HW_SCREEN_DISPLAY:
    case DASHCDG_HW_SCREEN_AUDIO_LAB:
        r = (uint8_t)(80 * breathe);
        g = (uint8_t)(40 * breathe);
        b = (uint8_t)(140 * breathe);
        break;
    case DASHCDG_HW_SCREEN_KARAOKE:
        if (s_cdg_stream_ok) {
            r = (uint8_t)(10 * breathe);
            g = (uint8_t)(200 * breathe);
            b = (uint8_t)(140 * breathe);
        } else {
            r = (uint8_t)(160 * breathe);
            g = (uint8_t)(30 * breathe);
            b = (uint8_t)(120 * breathe);
        }
        break;
    default:
        break;
    }
    rgb_apply_status(r, g, b);
}

static void hw_task(void *arg)
{
    (void)arg;
    uint64_t last_bat_ms = 0;

    while (1) {
        uint32_t tick_ms = beep_seq_active() ? BEEP_TICK_FAST_MS : HW_TICK_MS;
        vTaskDelay(pdMS_TO_TICKS(tick_ms));
        uint64_t now = dashcdg_clock_now_ms();

        if (now - last_bat_ms >= battery_sample_period_ms()) {
            last_bat_ms = now;
            sample_battery_update_cache();
        }
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(40)) == pdTRUE) {
            /* PENIRQ active low on touch (same pin as XPT2046 interrupt). */
            if (gpio_get_level(CYD_GPIO_TP_IRQ) == 0) {
                pm_bump_activity_locked(now);
            }

            pm_update_idle_locked(now);
            pm_backlight_fade_step_locked();

            bl_set_pct(s_bl_applied_pct);
            led_anim_frame(now);

            /* IO0: short press wakes from display sleep; long hold (~1.4s) starts sleep fade. */
            int lvl = gpio_get_level(DASHCDG_HW_GPIO_USER_BTN);
            if (lvl != 0) {
                s_btn_low_streak = 0;
                s_btn_block_until_hi = false;
            } else {
                if (s_btn_low_streak < 250) {
                    s_btn_low_streak++;
                }
                if (!s_btn_block_until_hi) {
                    /* Short tap: wake from panel sleep, or exit idle-dim to full brightness (same as touch). */
                    if ((s_pm_state == PM_SLEEP || s_pm_state == PM_DIM) && s_btn_low_streak == USER_BTN_WAKE_FRAMES) {
                        ESP_LOGI(TAG, "user button wake / exit dim (IO0)");
                        pm_bump_activity_locked(now);
                    } else if (s_pm_state != PM_SLEEP && s_pm_state != PM_WAKE_FADE && s_pm_state != PM_SLEEP_FADE &&
                               s_btn_low_streak == USER_BTN_SLEEP_FRAMES) {
                        ESP_LOGI(TAG, "user button sleep (IO0 hold)");
                        pm_request_manual_sleep_fade_locked(now);
                        s_btn_block_until_hi = true;
                    }
                }
            }

            beep_seq_tick_locked(now);
            xSemaphoreGive(s_mtx);
        }
    }
}

esp_err_t dashcdg_platform_hw_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) {
        return ESP_ERR_NO_MEM;
    }

    if (!dashcdg_vbat_sense_is_ready()) {
        esp_err_t v = dashcdg_vbat_sense_init();
        if (v != ESP_OK) {
            ESP_LOGW(TAG, "vbat init in platform_hw: %s", esp_err_to_name(v));
        }
    }

    gpio_reset_pin(DASHCDG_HW_GPIO_LCD_BL_PWM);
    gpio_reset_pin(DASHCDG_HW_GPIO_RGB_R);
    gpio_reset_pin(DASHCDG_HW_GPIO_RGB_G);
    gpio_reset_pin(DASHCDG_HW_GPIO_RGB_B);
    gpio_reset_pin(DASHCDG_HW_GPIO_AUDIO_PWM);

    ESP_RETURN_ON_ERROR(gpio_misc_init(), TAG, "gpio_misc");
    ESP_RETURN_ON_ERROR(ledc_install_rgb_bl(), TAG, "ledc rgb/bl");
    ESP_RETURN_ON_ERROR(ledc_install_beep(), TAG, "ledc beep");
    (void)i2c_bus_init();

    {
        uint8_t bl = 100;
        (void)dashcdg_badge_prefs_load_brightness(&bl);
        s_bl_user_pct = bl;
        s_bl_applied_pct = bl;
    }
    {
        uint8_t r_on = 1;
        uint8_t r_pct = 100;
        (void)dashcdg_badge_prefs_load_rgb_status(&r_on, &r_pct);
        s_rgb_status_enabled = (r_on != 0);
        s_rgb_status_pct = r_pct;
    }
    {
        uint8_t a_on = 1;
        (void)dashcdg_badge_prefs_load_auto_sleep(&a_on);
        s_auto_sleep_enabled = (a_on != 0);
    }
    {
        uint8_t bv = 85;
        (void)dashcdg_badge_prefs_load_beep_volume(&bv);
        if (bv < 5U) {
            bv = 5U;
        }
        if (bv > 100U) {
            bv = 100U;
        }
        s_beep_vol_pct = bv;
    }
    {
        uint8_t tb = 1;
        (void)dashcdg_badge_prefs_load_touch_beep(&tb);
        s_touch_beep_on = (tb != 0);
    }
    bl_set_pct(s_bl_applied_pct);
    if (!s_rgb_status_enabled) {
        rgb_pins_force_off_active_low();
    }
    s_activity_ms = dashcdg_clock_now_ms();
    s_pm_state = PM_ACTIVE;
    __atomic_store_n(&s_disp_pwrcmd, 0, __ATOMIC_SEQ_CST);
    s_screen = DASHCDG_HW_SCREEN_HOME;
    s_cdg_stream_ok = false;
    sample_battery_update_cache();

    BaseType_t ok = xTaskCreate(hw_task, "dashcdg_hw", HW_TASK_STACK, NULL, HW_TASK_PRIO, &s_hw_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }
    s_ready = true;
    ESP_LOGI(TAG, "platform hw task started (prio %d)", HW_TASK_PRIO);
    return ESP_OK;
}

bool dashcdg_platform_hw_is_ready(void)
{
    return s_ready;
}

void dashcdg_platform_hw_backlight_set_pct(uint8_t pct_0_100)
{
    if (pct_0_100 > 100U) {
        pct_0_100 = 100U;
    }
    if (pct_0_100 < 5U) {
        pct_0_100 = 5U;
    }
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_bl_user_pct = pct_0_100;
        if (s_pm_state == PM_ACTIVE) {
            s_bl_applied_pct = pct_0_100;
        }
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_set_screen(dashcdg_hw_screen_t s)
{
    if (!s_mtx) {
        return;
    }
    for (int attempt = 0; attempt < 4; attempt++) {
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(80)) == pdTRUE) {
            dashcdg_hw_screen_t prev = s_screen;
            s_screen = s;
            if (s == DASHCDG_HW_SCREEN_KARAOKE && prev != DASHCDG_HW_SCREEN_KARAOKE) {
                atomic_store_explicit(&s_karaoke_last_mcast_rx_ms, 0ULL, memory_order_relaxed);
                atomic_store_explicit(&s_karaoke_last_overlay_ms, 0ULL, memory_order_relaxed);
                s_karaoke_mcast_act_throttle_ms = 0ULL;
                /* Modem sleep adds multi-ms wake latency; multicast audio gaps read as choppy playout. */
                {
                    wifi_ps_type_t cur = WIFI_PS_NONE;
                    if (esp_wifi_get_ps(&cur) == ESP_OK) {
                        s_wifi_ps_karaoke_saved = cur;
                        s_wifi_ps_karaoke_saved_valid = true;
                    } else {
                        s_wifi_ps_karaoke_saved_valid = false;
                    }
                    (void)esp_wifi_set_ps(WIFI_PS_NONE);
                }
            } else if (prev == DASHCDG_HW_SCREEN_KARAOKE && s != DASHCDG_HW_SCREEN_KARAOKE) {
                if (s_wifi_ps_karaoke_saved_valid) {
                    (void)esp_wifi_set_ps(s_wifi_ps_karaoke_saved);
                    s_wifi_ps_karaoke_saved_valid = false;
                }
#if CONFIG_IDF_TARGET_ESP32
                if (!__atomic_load_n(&s_karaoke_pcm_streaming, __ATOMIC_RELAXED) &&
                    s_karaoke_dac_handle == NULL) {
                    (void)ledc_beep_audio_channel_attach_locked();
                }
#endif
            }
            pm_bump_activity_locked(dashcdg_clock_now_ms());
            xSemaphoreGive(s_mtx);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(3));
    }
}

void dashcdg_platform_hw_set_cdg_stream_ok(bool ok)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_cdg_stream_ok = ok;
        if (ok && s_screen == DASHCDG_HW_SCREEN_KARAOKE) {
            pm_bump_activity_locked(dashcdg_clock_now_ms());
        }
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_note_karaoke_mcast_rx(uint64_t rx_now_ms)
{
    atomic_store_explicit(&s_karaoke_last_mcast_rx_ms, rx_now_ms, memory_order_relaxed);
    if (!s_ready || !s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, 0) != pdTRUE) {
        return;
    }
    if (s_screen == DASHCDG_HW_SCREEN_KARAOKE) {
        if (s_karaoke_mcast_act_throttle_ms == 0ULL || (rx_now_ms - s_karaoke_mcast_act_throttle_ms) >= 250ULL) {
            s_karaoke_mcast_act_throttle_ms = rx_now_ms;
            pm_bump_activity_locked(rx_now_ms);
        }
    }
    xSemaphoreGive(s_mtx);
}

void dashcdg_platform_hw_note_karaoke_cdg_overlay_tick(uint64_t now_ms)
{
    atomic_store_explicit(&s_karaoke_last_overlay_ms, now_ms, memory_order_relaxed);
}

void dashcdg_platform_hw_touch_click(void)
{
    if (!s_ready || !s_mtx) {
        return;
    }
    uint64_t now = dashcdg_clock_now_ms();
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(40)) == pdTRUE) {
        bool was_sleep = (s_pm_state == PM_SLEEP);
        pm_bump_activity_locked(now);
        if (s_touch_beep_on && !was_sleep && s_screen != DASHCDG_HW_SCREEN_AUDIO_LAB) {
            if ((now - s_last_blip_queued_ms) >= BEEP_BLIP_MIN_GAP_MS) {
                if (beep_queue_copy_locked(k_blip_seq, (uint8_t)(sizeof(k_blip_seq) / sizeof(k_blip_seq[0])), BEEP_SEQ_BLIP,
                                           now)) {
                    s_last_blip_queued_ms = now;
                }
            }
        }
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_ui_sound_confirm_click(void)
{
    if (!s_ready || !s_mtx) {
        return;
    }
    uint64_t now = dashcdg_clock_now_ms();
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(40)) == pdTRUE) {
        bool was_sleep = (s_pm_state == PM_SLEEP);
        pm_bump_activity_locked(now);
        if (s_touch_beep_on && !was_sleep && s_screen != DASHCDG_HW_SCREEN_AUDIO_LAB) {
            if ((now - s_last_ui_queued_ms) >= BEEP_UI_MIN_GAP_MS) {
                if (beep_queue_copy_locked(k_ui_seq, (uint8_t)(sizeof(k_ui_seq) / sizeof(k_ui_seq[0])), BEEP_SEQ_UI, now)) {
                    s_last_ui_queued_ms = now;
                }
            }
        }
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_notify_activity(void)
{
    if (!s_ready || !s_mtx) {
        return;
    }
    uint64_t now = dashcdg_clock_now_ms();
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(25)) == pdTRUE) {
        pm_bump_activity_locked(now);
        xSemaphoreGive(s_mtx);
    }
}

int dashcdg_platform_hw_peek_display_power_cmd(void)
{
    return (int)__atomic_load_n(&s_disp_pwrcmd, __ATOMIC_SEQ_CST);
}

void dashcdg_platform_hw_ack_display_power_cmd(void)
{
    __atomic_store_n(&s_disp_pwrcmd, 0, __ATOMIC_SEQ_CST);
}

uint32_t dashcdg_platform_hw_consume_post_wake_ui_mask(void)
{
    return __atomic_exchange_n(&s_post_wake_ui_mask, 0U, __ATOMIC_ACQ_REL);
}

void dashcdg_platform_hw_set_rgb_status_enabled(bool on)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_rgb_status_enabled = on;
        if (!on) {
            rgb_pins_force_off_active_low();
        } else if (s_rgb_ledc_detached) {
            (void)rgb_status_ledc_channels_rebind();
            s_rgb_ledc_detached = false;
        }
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_set_rgb_status_brightness(uint8_t pct_5_100)
{
    if (pct_5_100 < 5U) {
        pct_5_100 = 5U;
    }
    if (pct_5_100 > 100U) {
        pct_5_100 = 100U;
    }
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_rgb_status_pct = pct_5_100;
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_set_auto_sleep_enabled(bool on)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_auto_sleep_enabled = on;
        if (!on) {
            pm_force_active_restore_locked();
        }
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_set_touch_beep_enabled(bool on)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_touch_beep_on = on;
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_set_beep_volume_pct(uint8_t pct_5_100)
{
    if (pct_5_100 < 5U) {
        pct_5_100 = 5U;
    }
    if (pct_5_100 > 100U) {
        pct_5_100 = 100U;
    }
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_beep_vol_pct = pct_5_100;
        xSemaphoreGive(s_mtx);
    }
}

bool dashcdg_platform_hw_lab_pcm_stream_begin(void)
{
    if (!s_mtx) {
        return false;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(120)) != pdTRUE) {
        return false;
    }
#if CONFIG_IDF_TARGET_ESP32
    if (s_karaoke_dac_handle != NULL) {
        xSemaphoreGive(s_mtx);
        return false;
    }
#endif
    /* Play demo: abort UI triad state; lab owns IO26 while streaming. */
    beep_seq_abort_locked();
#if CONFIG_IDF_TARGET_ESP32
    (void)ledc_stop(LEDC_MODE, LEDC_CH_AUDIO, 0);
    /* DMA descriptor buf_size must satisfy `write_bytes * 2 >= 2 * buf_size` (16-bit slot expand),
     * or IDF's `s_dac_wait_to_load_dma_data` halves alternate loads via a static split_flag
     * (motorboating / buzz). Match `LAB_DAC_PCM_CHUNK`. */
    dac_continuous_config_t dcfg = {
        .chan_mask = DASHCDG_HW_ESP32_DAC_LINE_CHANNEL_MASK,
        .desc_num = 8,
        .buf_size = LAB_DAC_PCM_CHUNK,
        .freq_hz = (uint32_t)DASHCDG_LAB_PCM_FS_HZ,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    esp_err_t de = dac_continuous_new_channels(&dcfg, &s_dac_lab_handle);
    if (de != ESP_OK) {
        ESP_LOGE(TAG, "dac_continuous_new_channels: %s", esp_err_to_name(de));
        s_dac_lab_handle = NULL;
        (void)ledc_beep_audio_channel_attach_locked();
        xSemaphoreGive(s_mtx);
        return false;
    }
    de = dac_continuous_enable(s_dac_lab_handle);
    if (de != ESP_OK) {
        ESP_LOGE(TAG, "dac_continuous_enable: %s", esp_err_to_name(de));
        (void)dac_continuous_disable(s_dac_lab_handle);
        (void)dac_continuous_del_channels(s_dac_lab_handle);
        s_dac_lab_handle = NULL;
        (void)ledc_beep_audio_channel_attach_locked();
        xSemaphoreGive(s_mtx);
        return false;
    }
    s_dac_lab_fill = 0U;
    s_dac_lp_u8 = 128U;
    __atomic_store_n(&s_lab_pcm_streaming, true, __ATOMIC_RELEASE);
    amp_set_run(true);
#else
    __atomic_store_n(&s_lab_pcm_streaming, true, __ATOMIC_RELEASE);
    (void)ledc_set_freq(LEDC_MODE, LEDC_TIMER_BEEP, 24000U);
    amp_set_run(true);
    ledc_set_duty(LEDC_MODE, LEDC_CH_AUDIO, 128U);
    ledc_update_duty(LEDC_MODE, LEDC_CH_AUDIO);
#endif
    xSemaphoreGive(s_mtx);
    return true;
}

bool dashcdg_platform_hw_lab_pcm_is_streaming(void)
{
    return __atomic_load_n(&s_lab_pcm_streaming, __ATOMIC_RELAXED);
}

void dashcdg_platform_hw_lab_pcm_stream_end(void)
{
    /* Always clear first: if mutex wait fails, lab stops pushing but a stuck "streaming" flag
     * would silence UI beeps forever (beep_seq_tick returns early while streaming is true). */
    __atomic_store_n(&s_lab_pcm_streaming, false, __ATOMIC_RELEASE);
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(120)) != pdTRUE) {
        return;
    }
#if CONFIG_IDF_TARGET_ESP32
    lab_dac_flush_stop_and_ledc_restore_locked();
#endif
    beep_seq_abort_locked();
    beep_mute_locked();
    xSemaphoreGive(s_mtx);
}

#if CONFIG_IDF_TARGET_ESP32
/**
 * Map PWM-style ~mid duty to DAC line level: modest AC gain + soft clip, then light low-pass
 * to reduce zipper / harsh clipping distortion at 24 kHz.
 */
static uint8_t lab_pcm_prepare_dac_u8(uint8_t pwm_style_u8)
{
    int32_t c = (int32_t)pwm_style_u8 - 128;
    c = (c * 20) / 10; /* 2.0x */
    if (c > 100) {
        c = 100 + (c - 100) / 4; /* soft knee */
    }
    if (c < -100) {
        c = -100 + (c + 100) / 4;
    }
    if (c > 115) {
        c = 115;
    }
    if (c < -115) {
        c = -115;
    }
    int32_t t = 128 + c;
    if (t < 10) {
        t = 10;
    }
    if (t > 245) {
        t = 245;
    }
    uint8_t tgt = (uint8_t)t;
    uint8_t out = (uint8_t)(((uint16_t)s_dac_lp_u8 * 3U + (uint16_t)tgt + 2U) / 4U);
    s_dac_lp_u8 = out;
    return out;
}
#endif

void dashcdg_platform_hw_lab_pcm_push_u8(uint8_t duty_u8)
{
    if (!__atomic_load_n(&s_lab_pcm_streaming, __ATOMIC_RELAXED)) {
        return;
    }
    amp_set_run(true);
#if CONFIG_IDF_TARGET_ESP32
    if (s_dac_lab_handle != NULL) {
        s_dac_lab_chunk[s_dac_lab_fill++] = lab_pcm_prepare_dac_u8(duty_u8);
        if (s_dac_lab_fill >= LAB_DAC_PCM_CHUNK) {
            (void)dac_continuous_write(s_dac_lab_handle, s_dac_lab_chunk, LAB_DAC_PCM_CHUNK, NULL, -1);
            s_dac_lab_fill = 0U;
        }
        return;
    }
#endif
    ledc_set_duty(LEDC_MODE, LEDC_CH_AUDIO, duty_u8);
    ledc_update_duty(LEDC_MODE, LEDC_CH_AUDIO);
}

uint8_t dashcdg_platform_hw_get_beep_volume_pct(void)
{
    /* Lab PCM runs in a tight loop; do not block on `s_mtx` here (UI/beep may hold it). */
    return s_beep_vol_pct;
}

esp_err_t dashcdg_platform_hw_battery_read(int *out_raw, int *out_pin_mv, int *out_vbat_mv)
{
    if (!s_ready || !s_mtx) {
        return dashcdg_vbat_sense_read(out_raw, out_pin_mv, out_vbat_mv);
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(80)) != pdTRUE) {
        return dashcdg_vbat_sense_read(out_raw, out_pin_mv, out_vbat_mv);
    }
    if (!s_bat_cache_valid) {
        xSemaphoreGive(s_mtx);
        return dashcdg_vbat_sense_read(out_raw, out_pin_mv, out_vbat_mv);
    }
    if (out_raw) {
        *out_raw = s_bat_raw;
    }
    if (out_pin_mv) {
        *out_pin_mv = s_bat_pin_mv;
    }
    if (out_vbat_mv) {
        *out_vbat_mv = s_bat_vbat_mv;
    }
    (void)s_bat_sample_ms;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}
