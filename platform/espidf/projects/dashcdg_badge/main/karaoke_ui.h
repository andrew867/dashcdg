#pragma once

#include "esp_err.h"
#include "lvgl.h"

esp_err_t dashcdg_karaoke_ui_present(lv_disp_t *disp);

/** Stop karaoke timers / modal before replacing the screen (nav_home calls this). */
void dashcdg_karaoke_ui_teardown(void);

/**
 * Re-read karaoke decode prefs from NV and refresh audio-only vs CDG stage layout once.
 * Call after `dashcdg_badge_rx_set_decode_enabled` from settings (decode toggles); no-op if karaoke
 * UI is not active. Must not be used on every LVGL tick — prefs change only from NV writes.
 */
void dashcdg_karaoke_ui_sync_decode_layout_from_prefs(void);
