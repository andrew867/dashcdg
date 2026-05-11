/*
 * Deferred SPI RGB565 band blitter (FreeRTOS worker). See badge_sp_blit_worker.h.
 */
#include "badge_sp_blit_worker.h"

#include <string.h>

#include "display_lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "badge_rx.h"

static const char *TAG = "sp_blit";

#define SP_BAND_W    DASHCDG_BADGE_RX_VISIBLE_W
#define SP_BAND_H    DASHCDG_BADGE_RX_BLIT_BAND_H
#define SP_BAND_PIX  ((size_t)SP_BAND_W * (size_t)SP_BAND_H)

typedef struct {
    uint8_t slot;
    int16_t x0;
    int16_t y0;
    int16_t w;
    int16_t h;
} sp_blit_msg_t;

static uint16_t s_pool[CONFIG_DASHCDG_BADGE_SP_BLIT_POOL_SLOTS][SP_BAND_PIX];
static QueueHandle_t s_free_slot_q;
static QueueHandle_t s_work_q;
static TaskHandle_t s_worker_task;
static uint8_t s_inited;

static void sp_blit_worker_task(void *arg)
{
    (void)arg;
    sp_blit_msg_t m;

    for (;;) {
        if (xQueueReceive(s_work_q, &m, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (m.w <= 0 || m.h <= 0 || m.slot >= CONFIG_DASHCDG_BADGE_SP_BLIT_POOL_SLOTS) {
            continue;
        }
        {
            const size_t need = (size_t)m.w * (size_t)m.h;

            if (need > SP_BAND_PIX) {
                ESP_LOGW(TAG, "band too large %dx%d", (int)m.w, (int)m.h);
            } else {
                const int64_t t0 = esp_timer_get_time();
                uint16_t *px = s_pool[m.slot];

                /* This task is not the LVGL task: take the port lock so SPI does not race flush/DMA. */
                if (lvgl_port_lock(3000)) {
                    (void)dashcdg_display_blit_rgb565_lv_area((int)m.x0, (int)m.y0, (int)m.w, (int)m.h, px);
                    esp_rom_delay_us((uint32_t)DASHCDG_BADGE_RX_PANEL_BAND_SETTLE_US);
                    lvgl_port_unlock();
                } else {
                    ESP_LOGW(TAG, "lvgl_port_lock timeout; band drop %dx%d@%d,%d", (int)m.w, (int)m.h, (int)m.x0,
                             (int)m.y0);
                }
                {
                    int64_t dt = esp_timer_get_time() - t0;

                    if (dt > 0 && dt < 2000000) {
                        dashcdg_badge_rx_perf_note_sp_blit_band_us((uint32_t)dt);
                    }
                }
            }
        }
        (void)xQueueSend(s_free_slot_q, &m.slot, 0);
    }
}

void dashcdg_sp_blit_worker_init(void)
{
    if (s_inited) {
        return;
    }
    if (!dashcdg_display_lcd_panel()) {
        ESP_LOGW(TAG, "panel not ready; deferred blit worker not started");
        return;
    }

    s_free_slot_q = xQueueCreate((UBaseType_t)CONFIG_DASHCDG_BADGE_SP_BLIT_POOL_SLOTS, sizeof(uint8_t));
    s_work_q = xQueueCreate((UBaseType_t)CONFIG_DASHCDG_BADGE_SP_BLIT_POOL_SLOTS * 4U, sizeof(sp_blit_msg_t));
    if (s_free_slot_q == NULL || s_work_q == NULL) {
        ESP_LOGE(TAG, "queue alloc failed");
        return;
    }
    for (uint8_t i = 0; i < (uint8_t)CONFIG_DASHCDG_BADGE_SP_BLIT_POOL_SLOTS; ++i) {
        (void)xQueueSend(s_free_slot_q, &i, 0);
    }

    const uint32_t stack_words = 4096U;
    if (xTaskCreatePinnedToCore(sp_blit_worker_task, "sp_blit", stack_words, NULL,
                                (UBaseType_t)CONFIG_DASHCDG_BADGE_SP_BLIT_TASK_PRIO, &s_worker_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
        return;
    }
    s_inited = 1;
    ESP_LOGI(TAG, "deferred SPI blit worker up (slots=%d prio=%d)", CONFIG_DASHCDG_BADGE_SP_BLIT_POOL_SLOTS,
             CONFIG_DASHCDG_BADGE_SP_BLIT_TASK_PRIO);
}

void dashcdg_sp_blit_worker_try_init(void)
{
    if (s_inited) {
        return;
    }
    dashcdg_sp_blit_worker_init();
}

esp_err_t dashcdg_sp_blit_enqueue_band(int x0, int y0, int w, int h, const uint16_t *src_rgb565,
                                       TickType_t wait_free_slot_ticks)
{
    uint8_t slot;
    sp_blit_msg_t m;

    if (!s_inited || src_rgb565 == NULL || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    {
        const size_t need = (size_t)w * (size_t)h;

        if (need > SP_BAND_PIX) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (xQueueReceive(s_free_slot_q, &slot, wait_free_slot_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(s_pool[slot], src_rgb565, (size_t)w * (size_t)h * sizeof(uint16_t));
    m.slot = slot;
    m.x0 = (int16_t)x0;
    m.y0 = (int16_t)y0;
    m.w = (int16_t)w;
    m.h = (int16_t)h;
    if (xQueueSend(s_work_q, &m, 0) != pdTRUE) {
        (void)xQueueSend(s_free_slot_q, &slot, 0);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
