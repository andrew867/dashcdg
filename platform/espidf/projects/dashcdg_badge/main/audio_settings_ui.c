/*
 * Setup: speaker level + touch beep NVS (Mary demo stays in Applications -> Audio lab).
 */
#include <stdio.h>

#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "audio_settings_ui.h"
#include "badge_prefs.h"
#include "display_lvgl.h"
#include "home_ui.h"
#include "nav.h"
#include "platform_hw.h"
#include "wifi_touch_ui.h"

static const char *TAG = "audio_settings_ui";

static int snap_pct_step5(int v)
{
    if (v <= 5) {
        return 5;
    }
    if (v >= 100) {
        return 100;
    }
    int r = ((v + 2) / 5) * 5;
    if (r < 5) {
        r = 5;
    }
    if (r > 100) {
        r = 100;
    }
    return r;
}

static void on_pct_slider_snap(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    int s = snap_pct_step5(v);
    if (s != v) {
        lv_slider_set_value(sl, (int32_t)s, LV_ANIM_OFF);
    }
}

static void ui_no_scroll(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void on_back(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_nav_settings(disp);
    }
}

static void on_vol_changed(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    lv_obj_t *lbl = lv_event_get_user_data(e);
    int v = snap_pct_step5((int)lv_slider_get_value(sl));
    dashcdg_platform_hw_notify_activity();
    dashcdg_platform_hw_set_beep_volume_pct((uint8_t)v);
    (void)dashcdg_badge_prefs_save_beep_volume((uint8_t)v);
    if (lbl) {
        char buf[56];
        snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MAX "  %d%%  saved", v);
        lv_label_set_text(lbl, buf);
    }
}

static void on_touch_beep_sw(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dashcdg_platform_hw_notify_activity();
    dashcdg_platform_hw_set_touch_beep_enabled(on);
    (void)dashcdg_badge_prefs_save_touch_beep(on ? 1U : 0U);
}

esp_err_t dashcdg_audio_settings_ui_present(lv_disp_t *disp)
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
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x060908), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *outer = lv_obj_create(scr);
    lv_obj_set_size(outer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(outer, 8, 0);
    lv_obj_set_style_border_width(outer, 0, 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(outer, lv_color_hex(0x060908), 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(outer, 8, 0);
    ui_no_scroll(outer);

    lv_obj_t *top = lv_obj_create(outer);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_height(top, 40);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_no_scroll(top);

    lv_obj_t *b_back = lv_button_create(top);
    lv_obj_set_width(b_back, 88);
    lv_obj_t *lb = lv_label_create(b_back);
    lv_label_set_text(lb, LV_SYMBOL_LEFT " cfg");
    lv_obj_center(lb);
    lv_obj_add_event_cb(b_back, on_back, LV_EVENT_CLICKED, disp);

    lv_obj_t *scroll = lv_obj_create(outer);
    lv_obj_set_width(scroll, lv_pct(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_style_pad_all(scroll, 0, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(scroll, 12, 0);

    lv_obj_t *title = lv_label_create(scroll);
    lv_label_set_text(title, LV_SYMBOL_AUDIO "  Audio / touch");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe8f0ec), 0);

    lv_obj_t *hint = lv_label_create(scroll);
    lv_label_set_text(hint, "Mary demo: Applications -> Audio lab.");
    lv_obj_set_width(hint, lv_pct(98));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x6a7d74), 0);
    lv_obj_set_style_text_opa(hint, LV_OPA_COVER, 0);

    lv_obj_t *box = lv_obj_create(scroll);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x1e3a30), 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0a1012), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 12, 0);
    ui_no_scroll(box);

    lv_obj_t *vol_sec = lv_label_create(box);
    lv_label_set_text(vol_sec, "SPEAKER LEVEL");
    lv_obj_set_style_text_color(vol_sec, lv_color_hex(0x7a8f86), 0);
    lv_obj_set_style_text_opa(vol_sec, LV_OPA_90, 0);

    uint8_t beep_pct = 85;
    (void)dashcdg_badge_prefs_load_beep_volume(&beep_pct);
    beep_pct = (uint8_t)snap_pct_step5((int)beep_pct);
    dashcdg_platform_hw_set_beep_volume_pct(beep_pct);

    lv_obj_t *vol_lbl = lv_label_create(box);
    {
        char buf[56];
        snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MAX "  %u%%  saved", (unsigned)beep_pct);
        lv_label_set_text(vol_lbl, buf);
    }
    lv_obj_set_style_text_color(vol_lbl, lv_color_hex(0xc4ddd0), 0);

    lv_obj_t *vol_sl = lv_slider_create(box);
    lv_obj_set_width(vol_sl, lv_pct(100));
    lv_slider_set_range(vol_sl, 5, 100);
    lv_slider_set_value(vol_sl, (int32_t)beep_pct, LV_ANIM_OFF);
    lv_obj_add_event_cb(vol_sl, on_pct_slider_snap, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(vol_sl, on_vol_changed, LV_EVENT_VALUE_CHANGED, vol_lbl);

    lv_obj_t *touch_sec = lv_label_create(box);
    lv_label_set_text(touch_sec, "TOUCH BEEPS");
    lv_obj_set_style_text_color(touch_sec, lv_color_hex(0x7a8f86), 0);
    lv_obj_set_style_text_opa(touch_sec, LV_OPA_90, 0);

    lv_obj_t *touch_row = lv_obj_create(box);
    lv_obj_set_width(touch_row, lv_pct(100));
    lv_obj_set_height(touch_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(touch_row, 0, 0);
    lv_obj_set_style_border_width(touch_row, 0, 0);
    lv_obj_set_style_bg_opa(touch_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(touch_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(touch_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_no_scroll(touch_row);

    lv_obj_t *touch_lbl = lv_label_create(touch_row);
    lv_label_set_text(touch_lbl, "Chirps on buttons");
    lv_obj_set_style_text_color(touch_lbl, lv_color_hex(0xaabbcc), 0);

    lv_obj_t *touch_sw = lv_switch_create(touch_row);
    {
        uint8_t tb = 1;
        (void)dashcdg_badge_prefs_load_touch_beep(&tb);
        if (tb != 0) {
            lv_obj_add_state(touch_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(touch_sw, LV_STATE_CHECKED);
        }
    }
    lv_obj_add_event_cb(touch_sw, on_touch_beep_sw, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_update_layout(outer);
    lv_obj_invalidate(scr);
    lvgl_port_unlock();

    dashcdg_platform_hw_set_screen(DASHCDG_HW_SCREEN_SETTINGS);
    return ESP_OK;
}
