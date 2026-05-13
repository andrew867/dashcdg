#pragma once

#include <stdint.h>

#include "esp_err.h"

/**
 * Background stats/telemetry/housekeeping task.
 *
 * Purpose:
 * - Move 1 Hz UART emission and v4_rx_stats TX off the `badge_rx` hot loop.
 * - Provide a safe home for periodic housekeeping (e.g. IGMP refresh) that must not block media.
 *
 * The stats task must never call LVGL APIs.
 */

esp_err_t dashcdg_badge_stats_init(void);

/**
 * Wake the stats task to run a housekeeping tick sooner than its normal cadence.
 * Non-blocking; safe from any task.
 */
void dashcdg_badge_stats_kick(void);

