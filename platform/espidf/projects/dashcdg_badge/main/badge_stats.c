#include "badge_stats.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"

#include "dashcdg/media_clock.h"

#include "badge_exec.h"
#include "badge_rx.h"

static const char *TAG = "badge_stats";

#ifndef BADGE_STATS_TASK_STACK
#define BADGE_STATS_TASK_STACK 3072
#endif

#ifndef BADGE_STATS_TASK_PRIO
#define BADGE_STATS_TASK_PRIO 3
#endif

#if !CONFIG_FREERTOS_UNICORE
/* Keep low-rate housekeeping off the RX/audio core. */
#define BADGE_STATS_TASK_CORE 0
#endif

static TaskHandle_t s_stats_task;
static uint64_t s_next_due_ms;

static void badge_stats_task_fn(void *arg)
{
    (void)arg;

    s_next_due_ms = 0U;
    for (;;) {
        /*
         * Sleep until either the next due time, or a kick. Kicks are best-effort; missed kicks
         * just mean we run on the next periodic tick.
         */
        const uint64_t now_ms = dashcdg_clock_now_ms();
        uint64_t wait_ms = 0U;

        if (s_next_due_ms == 0U || now_ms >= s_next_due_ms) {
            wait_ms = 0U;
        } else {
            wait_ms = s_next_due_ms - now_ms;
        }

        if (wait_ms > 0U) {
            /* Notify can wake early; timeout keeps cadence bounded if notifications are lost. */
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((uint32_t)wait_ms));
        } else {
            /* Yield if we're already due, but still allow kicks to coalesce. */
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        }

        {
            const uint64_t tick_now_ms = dashcdg_clock_now_ms();

            dashcdg_badge_exec_task_heartbeat("badge_stats");
            dashcdg_badge_exec_task_progress("badge_stats");

            /*
             * All periodic work lives behind this one call. It must not assume it's running on
             * the RX hot loop.
             */
            dashcdg_badge_rx_housekeeping_tick(tick_now_ms);

            /* Default cadence: 1 Hz (RX config may choose to internally no-op/throttle). */
            s_next_due_ms = tick_now_ms + 1000ULL;
        }
    }
}

esp_err_t dashcdg_badge_stats_init(void)
{
    if (s_stats_task != NULL) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(badge_stats_task_fn, "badge_stats", BADGE_STATS_TASK_STACK, NULL,
                                BADGE_STATS_TASK_PRIO, &s_stats_task);
    if (ok != pdPASS) {
        s_stats_task = NULL;
        ESP_LOGE(TAG, "xTaskCreate badge_stats failed");
        return ESP_FAIL;
    }
    (void)dashcdg_badge_exec_register_task("badge_stats", s_stats_task, (int)BADGE_STATS_TASK_PRIO,
                                          -1,
                                          (uint32_t)BADGE_STATS_TASK_STACK);
    return ESP_OK;
}

void dashcdg_badge_stats_kick(void)
{
    if (s_stats_task == NULL) {
        return;
    }
    (void)xTaskNotifyGive(s_stats_task);
}
