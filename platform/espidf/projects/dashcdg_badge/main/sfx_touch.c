#include "sfx_touch.h"

#include <math.h>
#include <string.h>

#include "audio_mgr.h"
#include "badge_exec.h"
#include "platform_hw.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

static const char *TAG = "sfx_touch";

typedef enum {
    SFX_EVT_TOUCH = 1,
} sfx_evt_t;

static QueueHandle_t s_evt_q;
static TaskHandle_t s_task;
static lv_obj_t *s_attached_screen;
static uint64_t s_last_touch_ms;

enum {
    SFX_TOUCH_FS = 24000,
    SFX_TOUCH_MS = 80,
    SFX_TOUCH_SAMPLES = (SFX_TOUCH_FS * SFX_TOUCH_MS) / 1000,
};

static void sfx_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        sfx_evt_t evt;
        if (xQueueReceive(s_evt_q, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (evt != SFX_EVT_TOUCH) {
            continue;
        }

        /* Best-effort: do not crash if audio_mgr init fails. */
        (void)dashcdg_audio_mgr_init();
        if (!dashcdg_platform_hw_get_touch_beep_enabled() ||
                !dashcdg_audio_mgr_can_play_sfx_nominal_hz((uint32_t)SFX_TOUCH_FS)) {
            continue;
        }

        /* 80 ms chirp @ 24 kHz. */
        static int16_t pcm[SFX_TOUCH_SAMPLES];
        const float f0 = 880.0f;
        const float a = 7000.0f;

        for (int i = 0; i < SFX_TOUCH_SAMPLES; ++i) {
            float t = (float)i / (float)SFX_TOUCH_FS;
            float env = 1.0f;
            if (i < 64) {
                env = (float)i / 64.0f;
            } else if (i > (SFX_TOUCH_SAMPLES - 64)) {
                env = (float)(SFX_TOUCH_SAMPLES - i) / 64.0f;
            }
            float s = sinf(2.0f * (float)M_PI * f0 * t);
            int32_t v = (int32_t)(a * env * s);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            pcm[i] = (int16_t)v;
        }

        (void)dashcdg_audio_mgr_push_mono_s16((uint32_t)SFX_TOUCH_FS, pcm, (size_t)SFX_TOUCH_SAMPLES);
        dashcdg_badge_exec_task_progress("sfx_touch");
    }
}

static void on_screen_touch(lv_event_t *e)
{
    (void)e;

    if (!dashcdg_platform_hw_get_touch_beep_enabled() ||
            !dashcdg_audio_mgr_can_play_sfx_nominal_hz((uint32_t)SFX_TOUCH_FS)) {
        return;
    }

    uint64_t now_ms = (uint64_t)lv_tick_get();
    if (s_last_touch_ms != 0U && now_ms > s_last_touch_ms && (now_ms - s_last_touch_ms) < 120U) {
        return;
    }
    s_last_touch_ms = now_ms;

    if (s_evt_q == NULL) {
        return;
    }
    sfx_evt_t evt = SFX_EVT_TOUCH;
    (void)xQueueSend(s_evt_q, &evt, 0);
}

void dashcdg_sfx_touch_init(void)
{
    if (s_evt_q != NULL) {
        return;
    }
    s_evt_q = xQueueCreate(8, sizeof(sfx_evt_t));
    if (s_evt_q == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return;
    }
    if (xTaskCreate(sfx_task_fn, "sfx_touch", 3072, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        s_task = NULL;
        vQueueDelete(s_evt_q);
        s_evt_q = NULL;
        return;
    }
    (void)dashcdg_badge_exec_register_task("sfx_touch", s_task, 3, -1, 3072);
}

void dashcdg_sfx_touch_attach_to_active_screen(lv_disp_t *disp)
{
    if (disp == NULL) {
        return;
    }
    if (s_evt_q == NULL) {
        dashcdg_sfx_touch_init();
    }

    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    if (scr == NULL || scr == s_attached_screen) {
        return;
    }
    s_attached_screen = scr;
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, on_screen_touch, LV_EVENT_PRESSED, NULL);
}

