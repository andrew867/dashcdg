/*
 * Low-priority FreeRTOS task: RGB status LED (LEDC), backlight PWM (IO27), battery cache,
 * user button (IO0), SC8002B enable (IO4), PWM "chiptune" audio on IO26 (slow multi-note sequences,
 * sine-shaped duty envelope; mid-range carriers — high/fast modulation reads as hash on PWM).
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
#include "esp_check.h"
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

static bool s_rgb_status_enabled = true;
static uint8_t s_rgb_status_pct = 100;
static bool s_auto_sleep_enabled = true;

/** Stashed when panel commits sleep; LVGL consumes on wake (see `dashcdg_platform_hw_consume_post_wake_ui_mask`). */
static volatile uint32_t s_post_wake_ui_mask;

static int s_bat_raw;
static int s_bat_pin_mv;
static int s_bat_vbat_mv;
static uint64_t s_bat_sample_ms;

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
static uint8_t s_beep_vol_pct = 85;
/** LVGL UI tones (button triad, slider blip); power jingles ignore this. */
static bool s_touch_beep_on = true;

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

/** When true, `beep_seq_tick` yields IO26 to `dashcdg_platform_hw_lab_pcm_push_u8` (fixed-carrier PWM). */
static volatile bool s_lab_pcm_streaming;

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
        if (last_rx != 0ULL && now >= last_rx && (now - last_rx) < (uint64_t)DASHCDG_HW_IDLE_DIM_MS) {
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
    (void)beep_queue_copy_locked(k_sleep_seq, (uint8_t)(sizeof(k_sleep_seq) / sizeof(k_sleep_seq[0])), BEEP_SEQ_SLEEP, now);
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
        (void)beep_queue_copy_locked(k_wake_seq, (uint8_t)(sizeof(k_wake_seq) / sizeof(k_wake_seq[0])), BEEP_SEQ_WAKE, now);
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
        (void)beep_queue_copy_locked(k_sleep_seq, (uint8_t)(sizeof(k_sleep_seq) / sizeof(k_sleep_seq[0])), BEEP_SEQ_SLEEP, now);
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

static void rgb_set_u8(uint8_t r, uint8_t g, uint8_t b)
{
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
        rgb_set_u8(0, 0, 0);
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
    /* SC8002B: VDD on shutdown = shutdown. LOW = amp on. */
    gpio_set_level(DASHCDG_HW_GPIO_AMP_SHUTDOWN, run ? 0 : 1);
}

/** PWM to 0 only; does not touch amp shutdown (use between envelope samples while a jingle runs). */
static void beep_pwm_zero_locked(void)
{
    ledc_set_duty(LEDC_MODE, LEDC_CH_AUDIO, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CH_AUDIO);
}

/** Full mute: zero PWM and assert SC8002B shutdown (call when idle / sequence finished). */
static void beep_mute_locked(void)
{
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
    if (s_seq_len > 0U && s_seq_idx < s_seq_len) {
        while (s_seq_idx < s_seq_len && now >= s_seq_note_t1_ms) {
            s_seq_idx++;
            if (s_seq_idx >= s_seq_len) {
                s_seq_len = 0U;
                s_seq_kind = BEEP_SEQ_NONE;
                goto after_note_seq;
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
        return;
    }

after_note_seq:
    beep_mute_locked();
}

static bool beep_seq_active(void)
{
    return s_seq_len > 0U && s_seq_idx < s_seq_len;
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
        s_bat_raw = raw;
        s_bat_pin_mv = pin;
        s_bat_vbat_mv = vbat;
        s_bat_sample_ms = dashcdg_clock_now_ms();
        xSemaphoreGive(s_mtx);
    }
}

static void led_anim_frame(uint64_t now_ms)
{
    if (s_pm_state == PM_SLEEP) {
        /* Deep standby: very dim slow blue "breath" so the badge still feels alive. */
        if (!s_rgb_status_enabled) {
            rgb_set_u8(0, 0, 0);
            return;
        }
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

        if (now - last_bat_ms >= 400U) {
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
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        dashcdg_hw_screen_t prev = s_screen;
        s_screen = s;
        if (s == DASHCDG_HW_SCREEN_KARAOKE && prev != DASHCDG_HW_SCREEN_KARAOKE) {
            atomic_store_explicit(&s_karaoke_last_mcast_rx_ms, 0ULL, memory_order_relaxed);
            s_karaoke_mcast_act_throttle_ms = 0ULL;
        }
        pm_bump_activity_locked(dashcdg_clock_now_ms());
        xSemaphoreGive(s_mtx);
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

void dashcdg_platform_hw_touch_click(void)
{
    if (!s_ready || !s_mtx) {
        return;
    }
    uint64_t now = dashcdg_clock_now_ms();
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        bool was_sleep = (s_pm_state == PM_SLEEP);
        pm_bump_activity_locked(now);
        if (s_touch_beep_on && !was_sleep) {
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
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        bool was_sleep = (s_pm_state == PM_SLEEP);
        pm_bump_activity_locked(now);
        if (s_touch_beep_on && !was_sleep) {
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

void dashcdg_platform_hw_lab_pcm_stream_begin(void)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(120)) != pdTRUE) {
        return;
    }
    __atomic_store_n(&s_lab_pcm_streaming, true, __ATOMIC_RELEASE);
    /* High carrier so duty changes approximate PCM; above UI beep path 8 kHz cap (see `beep_apply_freq_duty_locked`). */
    (void)ledc_set_freq(LEDC_MODE, LEDC_TIMER_BEEP, 24000U);
    amp_set_run(true);
    ledc_set_duty(LEDC_MODE, LEDC_CH_AUDIO, 128U);
    ledc_update_duty(LEDC_MODE, LEDC_CH_AUDIO);
    xSemaphoreGive(s_mtx);
}

void dashcdg_platform_hw_lab_pcm_stream_end(void)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(120)) != pdTRUE) {
        return;
    }
    __atomic_store_n(&s_lab_pcm_streaming, false, __ATOMIC_RELEASE);
    beep_mute_locked();
    xSemaphoreGive(s_mtx);
}

void dashcdg_platform_hw_lab_pcm_push_u8(uint8_t duty_u8)
{
    if (!__atomic_load_n(&s_lab_pcm_streaming, __ATOMIC_RELAXED)) {
        return;
    }
    amp_set_run(true);
    ledc_set_duty(LEDC_MODE, LEDC_CH_AUDIO, duty_u8);
    ledc_update_duty(LEDC_MODE, LEDC_CH_AUDIO);
}

uint8_t dashcdg_platform_hw_get_beep_volume_pct(void)
{
    uint8_t v = 85;
    if (!s_mtx) {
        return v;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        v = s_beep_vol_pct;
        xSemaphoreGive(s_mtx);
    }
    return v;
}

esp_err_t dashcdg_platform_hw_battery_read(int *out_raw, int *out_pin_mv, int *out_vbat_mv)
{
    if (!s_ready || !s_mtx) {
        return dashcdg_vbat_sense_read(out_raw, out_pin_mv, out_vbat_mv);
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(80)) != pdTRUE) {
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
