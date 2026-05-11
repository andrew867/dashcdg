#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/**
 * Dedicated FreeRTOS task + RGB565 band pool: LVGL / RX producer copies pixels, worker calls
 * dashcdg_display_blit_rgb565_lv_area + panel settle delay. Reusable for any SPI partial updates
 * that must not hold badge_rx s_mtx across DMA.
 */
void dashcdg_sp_blit_worker_init(void);

/** If init was skipped (e.g. panel not ready), retry once the panel exists. Safe from LVGL/RX threads. */
void dashcdg_sp_blit_worker_try_init(void);

/**
 * Queue one band blit. Copies w*h RGB565 pixels from src (may be s_cdg_blit_scratch) into an
 * internal pool slot then posts to the worker.
 *
 * @param wait_free_slot_ticks 0 = do not block waiting for a free pool slot (drop if saturated).
 */
esp_err_t dashcdg_sp_blit_enqueue_band(int x0, int y0, int w, int h, const uint16_t *src_rgb565,
                                       TickType_t wait_free_slot_ticks);
