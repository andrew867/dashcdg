/*
 * Settings hub: Wi-Fi setup and touch calibration entry points.
 */
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "display_lvgl.h"
#include "home_ui.h"
#include "nav.h"
#include "settings_ui.h"
#include "touch_cal_ui.h"
#include "wifi_touch_ui.h"

static const char *TAG = "settings_ui";

static void on_back(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_nav_home(disp);
    }
}

static void on_wifi(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_nav_wifi(disp);
    }
}

static void cal_done_to_settings(lv_disp_t *disp)
{
    (void)dashcdg_settings_ui_present(disp);
}

static void cal_cancel_to_settings(lv_disp_t *disp)
{
    (void)dashcdg_settings_ui_present(disp);
}

static void on_touch_cal(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (!disp) {
        return;
    }
    (void)dashcdg_touch_cal_ui_present(disp, true, cal_done_to_settings, cal_cancel_to_settings);
}

esp_err_t dashcdg_settings_ui_present(lv_disp_t *disp)
{
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_ARG, TAG, "disp");

    dashcdg_home_ui_pause_status_updates();
    dashcdg_wifi_drop_lvgl_refs();

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    dashcdg_display_clear_top_layer(disp);

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x020403), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *outer = lv_obj_create(scr);
    lv_obj_set_size(outer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(outer, 8, 0);
    lv_obj_set_style_border_width(outer, 0, 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(outer, 12, 0);

    lv_obj_t *top = lv_obj_create(outer);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_height(top, 40);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *b_back = lv_button_create(top);
    lv_obj_set_width(b_back, 72);
    lv_obj_t *lb = lv_label_create(b_back);
    lv_label_set_text(lb, "Home");
    lv_obj_center(lb);
    lv_obj_add_event_cb(b_back, on_back, LV_EVENT_CLICKED, disp);

    lv_obj_t *title = lv_label_create(outer);
    lv_label_set_text(title, "> settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0x33ff99), 0);

    lv_obj_t *b_wifi = lv_button_create(outer);
    lv_obj_set_width(b_wifi, lv_pct(100));
    lv_obj_set_height(b_wifi, 44);
    lv_obj_set_style_bg_color(b_wifi, lv_color_hex(0x0a1410), 0);
    lv_obj_set_style_border_color(b_wifi, lv_color_hex(0x00aa55), 0);
    lv_obj_set_style_border_width(b_wifi, 1, 0);
    lv_obj_t *lw = lv_label_create(b_wifi);
    lv_label_set_text(lw, "[ 802.11 ]  Wi-Fi / net cfg");
    lv_obj_set_style_text_color(lw, lv_color_hex(0x66ffaa), 0);
    lv_obj_center(lw);
    lv_obj_add_event_cb(b_wifi, on_wifi, LV_EVENT_CLICKED, disp);

    lv_obj_t *b_cal = lv_button_create(outer);
    lv_obj_set_width(b_cal, lv_pct(100));
    lv_obj_set_height(b_cal, 44);
    lv_obj_set_style_bg_color(b_cal, lv_color_hex(0x0a1410), 0);
    lv_obj_set_style_border_color(b_cal, lv_color_hex(0x00aa55), 0);
    lv_obj_set_style_border_width(b_cal, 1, 0);
    lv_obj_t *lc = lv_label_create(b_cal);
    lv_label_set_text(lc, "[ touch ]  4-corner calibration");
    lv_obj_set_style_text_color(lc, lv_color_hex(0x66ffaa), 0);
    lv_obj_center(lc);
    lv_obj_add_event_cb(b_cal, on_touch_cal, LV_EVENT_CLICKED, disp);

    lv_obj_update_layout(outer);
    lv_obj_invalidate(scr);
    lvgl_port_unlock();

    return ESP_OK;
}
