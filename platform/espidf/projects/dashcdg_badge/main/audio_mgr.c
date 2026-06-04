#include "audio_mgr.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "badge_exec.h"
#include "platform_hw.h"

static const char *TAG = "audio_mgr";

#ifndef AUDIO_MGR_TASK_STACK
#define AUDIO_MGR_TASK_STACK 4096
#endif

#ifndef AUDIO_MGR_TASK_PRIO
/*
 * Real-time-ish: keep the DAC fed even when LVGL or Wi-Fi are busy.
 * Do not outrank the RX owner/LVGL/Wi-Fi; starving the decoder/producer side creates
 * audible discontinuities that sound like "missing samples".
 */
#define AUDIO_MGR_TASK_PRIO 8
#endif

typedef struct {
    uint32_t nom_hz;
    uint16_t samples; /* <= AUDIO_MGR_SAMPLES_MAX */
    uint16_t _pad;
    int16_t pcm[0];
} audio_mgr_frame_hdr_t;

#ifndef AUDIO_MGR_SAMPLES_MAX
/* Default: 20 ms @ 48 kHz mono. If callers submit larger buffers we chunk on enqueue. */
#define AUDIO_MGR_SAMPLES_MAX 960
#endif

#ifndef AUDIO_MGR_POOL_FRAMES
/* ~0.32s at 20 ms frames; keep heap footprint low on ESP32 DRAM. */
#define AUDIO_MGR_POOL_FRAMES 16
#endif

typedef struct {
    uint32_t nom_hz;
    uint16_t samples;
    uint16_t _pad;
    int16_t pcm[AUDIO_MGR_SAMPLES_MAX];
} audio_mgr_frame_t;

static TaskHandle_t s_task;
static QueueHandle_t s_free_q;
static QueueHandle_t s_fill_q;

/* Fully static pool/queues: keep heap available for dac_continuous_new_channels(). */
static audio_mgr_frame_t s_frames[AUDIO_MGR_POOL_FRAMES];
static StaticQueue_t s_free_q_struct;
static StaticQueue_t s_fill_q_struct;
static audio_mgr_frame_t *s_free_q_storage[AUDIO_MGR_POOL_FRAMES];
static audio_mgr_frame_t *s_fill_q_storage[AUDIO_MGR_POOL_FRAMES];
static dashcdg_audio_mgr_stats_t s_stats;
static uint32_t s_active_nom_hz;
static uint32_t s_queued_samples;

static void audio_mgr_task_fn(void *arg)
{
    (void)arg;

    for (;;) {
        /* Sleep until a producer pokes us, but still wake periodically for housekeeping. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));

        audio_mgr_frame_t *frame = NULL;
        dashcdg_badge_exec_task_heartbeat("audio_mgr");
        dashcdg_badge_exec_task_progress("audio_mgr");

        /* Non-blocking: never manufacture PCM here; only play what the producer provides. */
        (void)xQueueReceive(s_fill_q, &frame, 0);

        uint32_t nom_hz = s_stats.last_nom_hz;
        if (frame != NULL && frame->nom_hz != 0U) {
            nom_hz = frame->nom_hz;
            s_stats.last_nom_hz = frame->nom_hz;
        }

        if (nom_hz != 0U && nom_hz != s_active_nom_hz) {
            if (!dashcdg_platform_hw_lab_pcm_stream_matches_nominal_hz(nom_hz) &&
                    !dashcdg_platform_hw_karaoke_dac_begin_nominal_hz(nom_hz)) {
                /* Mirror RX's legacy defensive retry path. */
                dashcdg_platform_hw_karaoke_amp_arm_for_rx();
                if (!dashcdg_platform_hw_karaoke_dac_begin_nominal_hz(nom_hz)) {
                    s_stats.dac_begin_fail++;
                    if (frame != NULL) {
                        (void)xQueueSend(s_free_q, &frame, 0);
                    }
                    continue;
                }
            }
            s_active_nom_hz = nom_hz;
        }

        if (frame != NULL && frame->samples != 0U) {
            dashcdg_platform_hw_karaoke_dac_push_mono_s16(frame->pcm, (size_t)frame->samples);
            s_stats.frames_pushed++;
            s_stats.bytes_pushed += (uint32_t)frame->samples * (uint32_t)sizeof(int16_t);
            __atomic_fetch_sub(&s_queued_samples, (uint32_t)frame->samples, __ATOMIC_RELAXED);
        }
        if (frame != NULL) {
            (void)xQueueSend(s_free_q, &frame, 0);
        }
    }
}

esp_err_t dashcdg_audio_mgr_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_free_q = xQueueCreateStatic(
            AUDIO_MGR_POOL_FRAMES,
            sizeof(audio_mgr_frame_t *),
            (uint8_t *)s_free_q_storage,
            &s_free_q_struct);
    s_fill_q = xQueueCreateStatic(
            AUDIO_MGR_POOL_FRAMES,
            sizeof(audio_mgr_frame_t *),
            (uint8_t *)s_fill_q_storage,
            &s_fill_q_struct);
    if (s_free_q == NULL || s_fill_q == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < (size_t)AUDIO_MGR_POOL_FRAMES; i++) {
        audio_mgr_frame_t *f = &s_frames[i];
        memset(f, 0, sizeof(*f));
        (void)xQueueSend(s_free_q, &f, 0);
    }

    /*
     * Do not pin: pinning regressions can manifest as periodic DAC starvation depending on
     * where Wi-Fi/LVGL land on a given build.
     */
    BaseType_t ok = xTaskCreate(audio_mgr_task_fn, "audio_mgr", AUDIO_MGR_TASK_STACK, NULL, AUDIO_MGR_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "xTaskCreate audio_mgr failed");
        return ESP_FAIL;
    }
    (void)dashcdg_badge_exec_register_task("audio_mgr", s_task, (int)AUDIO_MGR_TASK_PRIO,
                                           (int8_t)-1,
                                           (uint32_t)AUDIO_MGR_TASK_STACK);
    memset(&s_stats, 0, sizeof(s_stats));
    s_active_nom_hz = 0U;
    return ESP_OK;
}

bool dashcdg_audio_mgr_push_mono_s16(uint32_t nom_hz, const int16_t *mono, size_t mono_samples)
{
    if (s_free_q == NULL || s_fill_q == NULL || mono == NULL || mono_samples == 0U) {
        return false;
    }

    bool any = false;
    size_t off = 0U;
    while (off < mono_samples) {
        size_t n = mono_samples - off;
        if (n > (size_t)AUDIO_MGR_SAMPLES_MAX) {
            n = (size_t)AUDIO_MGR_SAMPLES_MAX;
        }

        audio_mgr_frame_t *frame = NULL;
        if (xQueueReceive(s_free_q, &frame, 0) != pdTRUE || frame == NULL) {
            /*
             * Back-pressure policy: drop the oldest queued frame (keep latest audio) instead of
             * returning false and producing hard silence.
             */
            audio_mgr_frame_t *old = NULL;
            if (xQueueReceive(s_fill_q, &old, 0) == pdTRUE && old != NULL) {
                if (old->samples != 0U) {
                    __atomic_fetch_sub(&s_queued_samples, (uint32_t)old->samples, __ATOMIC_RELAXED);
                }
                frame = old;
                s_stats.pcm_drop_oldest++;
            } else {
                s_stats.pcm_drop_full++;
                return any;
            }
        }
        frame->nom_hz = nom_hz;
        frame->samples = (uint16_t)n;
        frame->_pad = 0U;
        memcpy(frame->pcm, mono + off, n * sizeof(int16_t));

        if (xQueueSend(s_fill_q, &frame, 0) != pdTRUE) {
            (void)xQueueSend(s_free_q, &frame, 0);
            s_stats.pcm_drop_full++;
            return any;
        }
        __atomic_fetch_add(&s_queued_samples, (uint32_t)n, __ATOMIC_RELAXED);
        any = true;
        off += n;
    }

    if (any && s_task != NULL) {
        (void)xTaskNotifyGive(s_task);
    }
    return any;
}

void dashcdg_audio_mgr_session_break(void)
{
    /* Owner-only semantics are within platform_hw; this call is safe from any task. */
    dashcdg_platform_hw_karaoke_dac_session_break();
}

void dashcdg_audio_mgr_stop(void)
{
    /*
     * Re-entry bug fix:
     * `audio_mgr` only opens the DAC when `nom_hz` changes vs `s_active_nom_hz`. After a stop(),
     * the DAC handle is torn down but `s_active_nom_hz` remained latched (typically 48 kHz), so
     * the next session would push PCM into a closed DAC forever (silent) until a codec/rate change
     * or reboot.
     */
    dashcdg_platform_hw_karaoke_dac_stop();
    s_active_nom_hz = 0U;
    __atomic_store_n(&s_queued_samples, 0U, __ATOMIC_RELAXED);
    /*
     * Best-effort: drop any queued frames so the next session starts cleanly (non-blocking).
     * This avoids stale PCM from a previous session being written after re-entry.
     */
    if (s_fill_q && s_free_q) {
        for (;;) {
            audio_mgr_frame_t *frame = NULL;
            if (xQueueReceive(s_fill_q, &frame, 0) != pdTRUE || frame == NULL) {
                break;
            }
            (void)xQueueSend(s_free_q, &frame, 0);
        }
    }
}

void dashcdg_audio_mgr_set_trim_ppm(int32_t ppm)
{
    dashcdg_platform_hw_karaoke_dac_set_trim_ppm(ppm);
}

bool dashcdg_audio_mgr_can_play_sfx_nominal_hz(uint32_t nom_hz)
{
    uint32_t active = __atomic_load_n(&s_active_nom_hz, __ATOMIC_RELAXED);

    return active == 0U || active == nom_hz;
}

void dashcdg_audio_mgr_get_stats(dashcdg_audio_mgr_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_stats;
    out->queued_samples = __atomic_load_n(&s_queued_samples, __ATOMIC_RELAXED);
}
