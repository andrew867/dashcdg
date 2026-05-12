/*
 * Audio lab: sine-wave demo tune ("Mary Had a Little Lamb") plus soft bass + echo.
 * ESP32: mono s16 → `dashcdg_platform_hw_karaoke_dac_push_mono_s16` (same `dac_continuous` path as CDG RX).
 * Other targets: PWM duty via `dashcdg_platform_hw_lab_pcm_push_u8`.
 */
#include "badge_lab_ym.h"

#include "badge_exec.h"
#include "platform_hw.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
#include "esp_attr.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static const char *TAG = "badge_lab_ym";

#define LAB_TASK_STACK  4096
/* Timer-driven PCM: prio only affects wake latency vs LVGL (no busy-spin on CPU). */
#define LAB_TASK_PRIO   8
#define LAB_FS_HZ       DASHCDG_LAB_PCM_FS_HZ
#define LAB_SAMPLE_US   (1000000u / LAB_FS_HZ)

#define PATTERN_LEN     32U
/** ~120 BPM sixteenths: 0.125 s per pattern step at Fs (`Fs/8`). */
#define TICKS_PER_STEP  (DASHCDG_LAB_PCM_FS_HZ / 8u)
/** Max samples per tight loop before yielding (backlog after preemption must not starve LVGL / WDT). */
#define LAB_MAX_BURST     2048U

#if CONFIG_IDF_TARGET_ESP32
/** Slightly wider than PWM-only path: DAC chain applies its own level / softening in `platform_hw.c`. */
#define LAB_SAMPLE_PEAK_CLAMP 72
#else
#define LAB_SAMPLE_PEAK_CLAMP 52
#endif

static TaskHandle_t s_task;
static esp_timer_handle_t s_pcm_tmr;
static volatile bool s_want_play;

#if CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
static void IRAM_ATTR lab_pcm_timer_cb(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    if (s_task != NULL) {
        vTaskNotifyGiveFromISR(s_task, &hpw);
    }
    if (hpw == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}
#else
static void lab_pcm_timer_cb(void *arg)
{
    (void)arg;
    if (s_task != NULL) {
        (void)xTaskNotifyGive(s_task);
    }
}
#endif

static uint32_t s_phase[3];
static uint32_t s_inc[3];
static uint32_t s_tick;
static uint32_t s_step_counter;
static uint32_t s_pat_pos;

/** Lead: "Mary Had a Little Lamb" in C (Hz, 0 = rest). */
static const uint16_t k_lead[PATTERN_LEN] = {
    330, 294, 262, 294, 330, 330, 330, 294, 294, 294, 330, 392, 392,
    330, 294, 262, 294, 330, 330, 330, 330, 294, 294, 330, 294, 262, 0, 0, 0, 0, 0, 0,
};

/** Bass: ~two octaves below lead (Hz, 0 = rest). */
static const uint16_t k_bass[PATTERN_LEN] = {
    82, 73, 65, 73, 82, 82, 82, 73, 73, 73, 82, 98, 98,
    82, 73, 65, 73, 82, 82, 82, 82, 73, 73, 82, 73, 65, 0, 0, 0, 0, 0, 0,
};

/** 1 = soft kick on phrase starts. */
static const uint8_t k_drum[PATTERN_LEN] = {
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static uint8_t s_kick_env;
static uint8_t s_snare_env;

/** Full-period sin LUT: round(sin(2π·i/256)·32767). */
static const int16_t k_sin_q15[256] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179, 6393, 5602, 4808, 4011, 3212, 2410, 1608, 804,
    0, -804, -1608, -2410, -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804,
};

static int32_t ym_sin_from_phase(uint32_t phase)
{
    unsigned i = (unsigned)(phase >> 24);
    int32_t s = (int32_t)k_sin_q15[i];
    return (s * 2730) >> 15;
}

static uint32_t hz_to_inc(uint16_t hz)
{
    if (hz == 0U) {
        return 0U;
    }
    /* 32-bit phase accumulator: increment per sample = hz / Fs * 2^32. */
    return (uint32_t)(((uint64_t)hz << 32) / (uint64_t)LAB_FS_HZ);
}

static void pattern_apply_step(void)
{
    uint16_t l = k_lead[s_pat_pos];
    uint16_t b = k_bass[s_pat_pos];
    uint8_t d = k_drum[s_pat_pos];

    s_inc[0] = hz_to_inc(l);
    s_inc[1] = hz_to_inc(b);
    /* High echo: fifth-ish double from lead two steps behind. */
    uint32_t echo_pos = (s_pat_pos + PATTERN_LEN - 2U) % PATTERN_LEN;
    uint16_t el = k_lead[echo_pos];
    if (el > 0U) {
        s_inc[2] = hz_to_inc((uint16_t)((el * 3U) / 2U)); /* ~perfect fifth */
    } else {
        s_inc[2] = 0U;
    }

    if (d == 1U) {
        s_kick_env = 22U;
    } else if (d == 2U) {
        s_snare_env = 28U;
    }
}

static void pattern_advance_if_needed(void)
{
    s_step_counter++;
    if (s_step_counter >= TICKS_PER_STEP) {
        s_step_counter = 0U;
        s_pat_pos = (s_pat_pos + 1U) % PATTERN_LEN;
        pattern_apply_step();
    }
}

static int32_t ym_tick(void)
{
    s_tick++;
    pattern_advance_if_needed();

    int32_t acc = 0;
    for (int i = 0; i < 3; i++) {
        if (s_inc[i] == 0U) {
            continue;
        }
        s_phase[i] += s_inc[i];
        int32_t sv = ym_sin_from_phase(s_phase[i]);
        if (i == 2) {
            sv = (sv * 13) / 32; /* echo channel quieter */
        }
        acc += sv;
    }
    acc /= 3;

    if (s_kick_env > 0U) {
        acc += (int32_t)s_kick_env * 95;
        s_kick_env--;
    }
    if (s_snare_env > 0U) {
        /* Hiss-ish: sign from phase LSB */
        int32_t hiss = ((s_tick & 1U) != 0U) ? 900 : -900;
        acc += (hiss * (int32_t)s_snare_env) / 28;
        s_snare_env--;
    }
    return acc;
}

static uint8_t lab_sample_to_duty(int32_t sample)
{
    uint8_t pct = dashcdg_platform_hw_get_beep_volume_pct();
    float p = (float)pct;
    if (p < 5.f) {
        p = 5.f;
    }
    if (p > 100.f) {
        p = 100.f;
    }
    float t = (p - 5.f) / 95.f;
    float g = powf(t, 3.4f);
    uint32_t peak = (uint32_t)(g * 255.f + 0.5f);
    if (peak < 1U && pct >= 6U) {
        peak = 1U;
    }
    if (peak > 255U) {
        peak = 255U;
    }

    /* Keep below full 8-bit swing: SC8002B + transducer; ESP32 DAC path scales further in platform_hw. */
    int32_t x = (sample * (int32_t)peak) / 4096;
    if (x > LAB_SAMPLE_PEAK_CLAMP) {
        x = LAB_SAMPLE_PEAK_CLAMP;
    }
    if (x < -LAB_SAMPLE_PEAK_CLAMP) {
        x = -LAB_SAMPLE_PEAK_CLAMP;
    }
    int32_t d = (int32_t)128 + x;
    if (d < 4) {
        d = 4;
    }
    if (d > 252) {
        d = 252;
    }
    return (uint8_t)d;
}

#if CONFIG_IDF_TARGET_ESP32
/** Maps mixer sample to s16 for `karaoke_dac_push` (~matches legacy duty swing vs `KARAOKE_DAC_PCM_GAIN_NUM`). */
static int16_t lab_acc_to_pcm16(int32_t sample)
{
    uint8_t pct = dashcdg_platform_hw_get_beep_volume_pct();
    float p = (float)pct;
    if (p < 5.f) {
        p = 5.f;
    }
    if (p > 100.f) {
        p = 100.f;
    }
    float t = (p - 5.f) / 95.f;
    float g = powf(t, 3.4f);
    uint32_t peak = (uint32_t)(g * 255.f + 0.5f);
    if (peak < 1U && pct >= 6U) {
        peak = 1U;
    }
    if (peak > 255U) {
        peak = 255U;
    }

    int32_t x = (sample * (int32_t)peak) / 4096;
    if (x > LAB_SAMPLE_PEAK_CLAMP) {
        x = LAB_SAMPLE_PEAK_CLAMP;
    }
    if (x < -LAB_SAMPLE_PEAK_CLAMP) {
        x = -LAB_SAMPLE_PEAK_CLAMP;
    }
    int32_t s = (x * 18842) / (int32_t)LAB_SAMPLE_PEAK_CLAMP;
    if (s > 32767) {
        s = 32767;
    }
    if (s < -32768) {
        s = -32768;
    }
    return (int16_t)s;
}
#endif

static void lab_task(void *arg)
{
    (void)arg;
    bool carrier_on = false;

    for (;;) {
        if (!s_want_play) {
            if (carrier_on) {
                if (s_pcm_tmr) {
                    (void)esp_timer_stop(s_pcm_tmr);
                }
                dashcdg_platform_hw_lab_pcm_stream_end();
                carrier_on = false;
            }
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }

        /* `play_set(true)` calls `lab_pcm_stream_end()` first; re-acquire streaming if we lost it. */
        bool streaming = dashcdg_platform_hw_lab_pcm_is_streaming();
        if (!carrier_on || !streaming) {
            if (carrier_on && !streaming) {
                if (s_pcm_tmr) {
                    (void)esp_timer_stop(s_pcm_tmr);
                }
                carrier_on = false;
            }
            if (!carrier_on) {
                dashcdg_badge_lab_ym_reset();
                if (!dashcdg_platform_hw_lab_pcm_stream_begin()) {
                    vTaskDelay(pdMS_TO_TICKS(2));
                    continue;
                }
                carrier_on = true;
                if (s_pcm_tmr && esp_timer_start_periodic(s_pcm_tmr, LAB_SAMPLE_US) != ESP_OK) {
                    ESP_LOGE(TAG, "esp_timer_start_periodic failed");
                }
            }
        }

        /* `ulTaskNotifyTake(pdTRUE, ...)` returns the pending count then clears it — process every tick. */
        uint32_t pending = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        if (!s_want_play) {
            if (s_pcm_tmr) {
                (void)esp_timer_stop(s_pcm_tmr);
            }
            if (carrier_on) {
                dashcdg_platform_hw_lab_pcm_stream_end();
                carrier_on = false;
            }
            continue;
        }
        if (!dashcdg_platform_hw_lab_pcm_is_streaming()) {
            if (s_pcm_tmr) {
                (void)esp_timer_stop(s_pcm_tmr);
            }
            carrier_on = false;
            continue;
        }
        if (pending == 0U) {
            /* Timeout: keep streaming path alive; timer may have stalled. */
            continue;
        }
        while (pending > 0U) {
            if (!s_want_play) {
                break;
            }
            if (!dashcdg_platform_hw_lab_pcm_is_streaming()) {
                break;
            }
            uint32_t chunk = (pending > LAB_MAX_BURST) ? LAB_MAX_BURST : pending;
            pending -= chunk;
            while (chunk-- > 0U) {
                int32_t s = ym_tick();
#if CONFIG_IDF_TARGET_ESP32
                int16_t pcm = lab_acc_to_pcm16(s);
                dashcdg_platform_hw_karaoke_dac_push_mono_s16(&pcm, 1U);
#else
                uint8_t d = lab_sample_to_duty(s);
                dashcdg_platform_hw_lab_pcm_push_u8(d);
#endif
            }
            if (pending > 0U) {
                taskYIELD();
            }
        }
    }
}

void dashcdg_badge_lab_ym_reset(void)
{
    memset(s_phase, 0, sizeof(s_phase));
    s_tick = 0U;
    s_step_counter = 0U;
    s_pat_pos = 0U;
    s_kick_env = 0U;
    s_snare_env = 0U;
    pattern_apply_step();
}

void dashcdg_badge_lab_ym_init(void)
{
    if (s_task != NULL) {
        return;
    }
    BaseType_t ok = xTaskCreate(lab_task, "dashcdg_lab_ym", LAB_TASK_STACK, NULL, LAB_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate lab_task failed");
        s_task = NULL;
        return;
    }
    (void)dashcdg_badge_exec_register_task("dashcdg_lab_ym", s_task,
                                           (uint8_t)LAB_TASK_PRIO, (int8_t)-1,
                                           (uint16_t)LAB_TASK_STACK);
    (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_AUDIO_LAB,
                                        DASHCDG_BADGE_EXEC_HEALTH_OK, "task_up");
    const esp_timer_create_args_t tcfg = {
        .callback = &lab_pcm_timer_cb,
        .arg = NULL,
#if CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
        .dispatch_method = ESP_TIMER_ISR,
#else
        .dispatch_method = ESP_TIMER_TASK,
#endif
        .name = "lab_pcm",
        .skip_unhandled_events = false,
    };
    if (esp_timer_create(&tcfg, &s_pcm_tmr) != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create lab_pcm failed");
        s_pcm_tmr = NULL;
    }
}

void dashcdg_badge_lab_ym_play_set(bool play)
{
    dashcdg_badge_lab_ym_init();
    if (play) {
        /* Ensure IO26 / streaming flag / seq state are sane before the lab task opens PCM again. */
        dashcdg_platform_hw_lab_pcm_stream_end();
        dashcdg_badge_lab_ym_reset();
    } else {
        /* Stop timer + streaming here so pause is immediate (lab task may be mid-burst on ulTaskNotifyTake). */
        if (s_pcm_tmr) {
            (void)esp_timer_stop(s_pcm_tmr);
        }
        dashcdg_platform_hw_lab_pcm_stream_end();
    }
    s_want_play = play;
    if (s_task != NULL) {
        (void)xTaskNotifyGive(s_task);
    }
}

void dashcdg_badge_lab_ym_stop(void)
{
    s_want_play = false;
    if (s_pcm_tmr) {
        (void)esp_timer_stop(s_pcm_tmr);
    }
    dashcdg_platform_hw_lab_pcm_stream_end();
    if (s_task) {
        (void)xTaskNotifyGive(s_task);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
