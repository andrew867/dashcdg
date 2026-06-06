#include "sfx_touch.h"

#include <math.h>
#include <stdbool.h>
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

static uint32_t sfx_touch_pick_nominal_hz(dashcdg_dac_route_owner_t owner)
{
    if (owner == DASHCDG_DAC_ROUTE_KARAOKE_RX) {
        return 48000U;
    }
    return (uint32_t)DASHCDG_LAB_PCM_FS_HZ;
}

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

        /*
         * Match the active DAC route. A 24 kHz UI chirp during 48 kHz karaoke would otherwise
         * tear down/reopen dac_continuous mid-song and can hit the low-heap NO_MEM cooldown path.
         */
        enum {
            MS = 80,
            FS_MAX = 48000,
            N_MAX = (FS_MAX * MS) / 1000,
        };
        const dashcdg_dac_route_owner_t owner = dashcdg_platform_hw_dac_route_owner();
        const bool standalone = (owner == DASHCDG_DAC_ROUTE_NONE);
        const uint32_t fs = sfx_touch_pick_nominal_hz(owner);
        const size_t n = (size_t)(((uint64_t)fs * (uint64_t)MS) / 1000ULL);
        static int16_t pcm[N_MAX];
        const float f0 = 880.0f;
        const float a = 7000.0f;

        for (size_t i = 0; i < n && i < (size_t)N_MAX; ++i) {
            float t = (float)i / (float)fs;
            float env = 1.0f;
            if (i < 64) {
                env = (float)i / 64.0f;
            } else if (i > (n - 64U)) {
                env = (float)(n - i) / 64.0f;
            }
            float s = sinf(2.0f * (float)M_PI * f0 * t);
            int32_t v = (int32_t)(a * env * s);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            pcm[i] = (int16_t)v;
        }

        if (dashcdg_audio_mgr_push_mono_s16(fs, pcm, n) && standalone) {
            vTaskDelay(pdMS_TO_TICKS(MS + 80U));
            dashcdg_audio_mgr_stats_t stats = {0};
            dashcdg_audio_mgr_get_stats(&stats);
            if (dashcdg_platform_hw_dac_route_owner() == DASHCDG_DAC_ROUTE_KARAOKE_RX &&
                stats.last_nom_hz == fs) {
                dashcdg_audio_mgr_stop();
            }
        }
        dashcdg_badge_exec_task_progress("sfx_touch");
    }
}

static void on_screen_touch(lv_event_t *e)
{
    (void)e;

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

