#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "badge_rx.h"
#include "display_ui.h"
#include "home_ui.h"
#include "karaoke_ui.h"
#include "nav.h"
#include "platform_hw.h"
#include "settings_ui.h"
#include "wifi_touch_ui.h"

void dashcdg_nav_home(lv_disp_t *disp)
{
    dashcdg_platform_hw_notify_activity();
    dashcdg_karaoke_ui_teardown();
    dashcdg_wifi_drop_lvgl_refs();
    (void)dashcdg_home_ui_present(disp);
    /* After home replaces the screen, karaoke CDG slot is gone; RX task stops (CDG+jitter heap kept). */
    dashcdg_badge_rx_stop();
}

void dashcdg_nav_settings(lv_disp_t *disp)
{
    dashcdg_platform_hw_notify_activity();
    dashcdg_home_ui_pause_status_updates();
    (void)dashcdg_settings_ui_present(disp);
}

void dashcdg_nav_display(lv_disp_t *disp)
{
    dashcdg_platform_hw_notify_activity();
    dashcdg_home_ui_pause_status_updates();
    (void)dashcdg_display_ui_present(disp);
}

void dashcdg_nav_wifi(lv_disp_t *disp)
{
    dashcdg_platform_hw_notify_activity();
    dashcdg_home_ui_pause_status_updates();
    (void)dashcdg_wifi_touch_ui_present(disp);
}

void dashcdg_nav_karaoke(lv_disp_t *disp)
{
    dashcdg_platform_hw_notify_activity();
    dashcdg_home_ui_pause_status_updates();
    dashcdg_wifi_drop_lvgl_refs();
    (void)dashcdg_karaoke_ui_present(disp);
}
