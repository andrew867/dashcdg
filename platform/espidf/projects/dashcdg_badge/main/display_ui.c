/*
 * Display lab: LCD backlight, rear RGB, idle power (NVS-backed). Speaker/touch: Settings or Audio lab.
 */
#include <stdio.h>

#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "badge_prefs.h"
#include "badge_ui_flair.h"
#include "display_lvgl.h"
#include "display_ui.h"
#include "home_ui.h"
#include "nav.h"
#include "platform_hw.h"
#include "wifi_touch_ui.h"

static const char *TAG = "display_ui";

/** NVS + platform use 5..100 in 5% steps; keep UI aligned. */
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

static void on_brightness_changed(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    lv_obj_t *lbl = lv_event_get_user_data(e);
    int v = snap_pct_step5((int)lv_slider_get_value(sl));
    dashcdg_platform_hw_notify_activity();
    dashcdg_platform_hw_backlight_set_pct((uint8_t)v);
    (void)dashcdg_badge_prefs_save_brightness((uint8_t)v);
    if (lbl) {
        char buf[56];
        snprintf(buf, sizeof(buf), LV_SYMBOL_TINT "  %d%%  saved", v);
        lv_label_set_text(lbl, buf);
    }
}

typedef struct {
    lv_obj_t *sw;
    lv_obj_t *sl;
    lv_obj_t *lbl_pct;
} rear_ui_t;

static rear_ui_t s_rear_ui;

typedef struct {
    lv_obj_t *sw;
    lv_obj_t *lbl;
} aslp_ui_t;

static aslp_ui_t s_aslp_ui;

static void on_rear_switch_changed(lv_event_t *e)
{
    (void)e;
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    int rp = snap_pct_step5((int)lv_slider_get_value(s_rear_ui.sl));
    dashcdg_platform_hw_notify_activity();
    dashcdg_platform_hw_set_rgb_status_enabled(on);
    dashcdg_platform_hw_set_rgb_status_brightness((uint8_t)rp);
    (void)dashcdg_badge_prefs_save_rgb_status(on ? 1U : 0U, (uint8_t)rp);
    if (on) {
        lv_obj_remove_state(s_rear_ui.sl, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_rear_ui.sl, LV_STATE_DISABLED);
    }
    if (s_rear_ui.lbl_pct) {
        char buf[56];
        snprintf(buf, sizeof(buf), LV_SYMBOL_IMAGE "  %d%%  saved", rp);
        lv_label_set_text(s_rear_ui.lbl_pct, buf);
    }
}

static void on_rear_slider_changed(lv_event_t *e)
{
    (void)e;
    if (!lv_obj_has_state(s_rear_ui.sw, LV_STATE_CHECKED)) {
        return;
    }
    int v = snap_pct_step5((int)lv_slider_get_value(s_rear_ui.sl));
    dashcdg_platform_hw_notify_activity();
    dashcdg_platform_hw_set_rgb_status_brightness((uint8_t)v);
    (void)dashcdg_badge_prefs_save_rgb_status(1U, (uint8_t)v);
    if (s_rear_ui.lbl_pct) {
        char buf[56];
        snprintf(buf, sizeof(buf), LV_SYMBOL_IMAGE "  %d%%  saved", v);
        lv_label_set_text(s_rear_ui.lbl_pct, buf);
    }
}

static void on_auto_sleep_switch_changed(lv_event_t *e)
{
    (void)e;
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    uint8_t onu = on ? 1U : 0U;
    dashcdg_platform_hw_notify_activity();
    dashcdg_platform_hw_set_auto_sleep_enabled(on);
    (void)dashcdg_badge_prefs_save_auto_sleep(onu);
    if (s_aslp_ui.lbl) {
        lv_label_set_text(s_aslp_ui.lbl,
                          on ? LV_SYMBOL_POWER "  Idle dim + sleep on  saved"
                             : LV_SYMBOL_POWER "  Idle dim + sleep off  saved");
    }
}

esp_err_t dashcdg_display_ui_present(lv_disp_t *disp)
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
    lv_label_set_text(title, "Display");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe8f0ec), 0);

    lv_obj_t *tag = lv_label_create(scroll);
    lv_label_set_text(tag, dashcdg_ui_flair_display_sub());
    lv_obj_set_width(tag, lv_pct(98));
    lv_label_set_long_mode(tag, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(tag, lv_color_hex(0x6a7d74), 0);
    lv_obj_set_style_text_opa(tag, LV_OPA_COVER, 0);

    uint8_t bp = 100;
    uint8_t rgb_on = 1;
    uint8_t rgb_pct = 100;
    uint8_t aslp_on = 1;
    (void)dashcdg_badge_prefs_load_brightness(&bp);
    (void)dashcdg_badge_prefs_load_rgb_status(&rgb_on, &rgb_pct);
    (void)dashcdg_badge_prefs_load_auto_sleep(&aslp_on);
    bp = (uint8_t)snap_pct_step5((int)bp);
    rgb_pct = (uint8_t)snap_pct_step5((int)rgb_pct);

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

    lv_obj_t *lcd_sec = lv_label_create(box);
    lv_label_set_text(lcd_sec, "BRIGHTNESS");
    lv_obj_set_style_text_color(lcd_sec, lv_color_hex(0x7a8f86), 0);
    lv_obj_set_style_text_opa(lcd_sec, LV_OPA_90, 0);

    lv_obj_t *lcd_h = lv_label_create(box);
    {
        char buf[56];
        snprintf(buf, sizeof(buf), LV_SYMBOL_TINT "  %u%%  saved", (unsigned)bp);
        lv_label_set_text(lcd_h, buf);
    }
    lv_obj_set_style_text_color(lcd_h, lv_color_hex(0xc4ddd0), 0);

    lv_obj_t *sl = lv_slider_create(box);
    lv_obj_set_width(sl, lv_pct(100));
    lv_slider_set_range(sl, 5, 100);
    lv_slider_set_value(sl, (int32_t)bp, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, on_pct_slider_snap, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, on_brightness_changed, LV_EVENT_VALUE_CHANGED, lcd_h);

    lv_obj_t *rgb_sec = lv_label_create(box);
    lv_label_set_text(rgb_sec, "REAR STATUS LED");
    lv_obj_set_style_text_color(rgb_sec, lv_color_hex(0x7a8f86), 0);
    lv_obj_set_style_text_opa(rgb_sec, LV_OPA_90, 0);

    lv_obj_t *rgb_caption = lv_label_create(box);
    lv_label_set_text(rgb_caption, "RGB on the back of the badge");
    lv_obj_set_style_text_color(rgb_caption, lv_color_hex(0x5c6e66), 0);
    lv_label_set_long_mode(rgb_caption, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(rgb_caption, lv_pct(100));

    lv_obj_t *rear_row = lv_obj_create(box);
    lv_obj_set_width(rear_row, lv_pct(100));
    lv_obj_set_height(rear_row, 40);
    lv_obj_set_style_pad_all(rear_row, 0, 0);
    lv_obj_set_style_border_width(rear_row, 0, 0);
    lv_obj_set_style_bg_opa(rear_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(rear_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rear_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(rear_row, 12, 0);
    ui_no_scroll(rear_row);

    lv_obj_t *rear_sw = lv_switch_create(rear_row);
    lv_obj_set_size(rear_sw, 52, 26);
    if (rgb_on) {
        lv_obj_add_state(rear_sw, LV_STATE_CHECKED);
    }
    lv_obj_t *rear_sw_lbl = lv_label_create(rear_row);
    lv_label_set_text(rear_sw_lbl, "LED");
    lv_obj_set_style_text_color(rear_sw_lbl, lv_color_hex(0xb8d4c8), 0);

    lv_obj_t *rgb_lbl = lv_label_create(box);
    {
        char buf[56];
        snprintf(buf, sizeof(buf), LV_SYMBOL_IMAGE "  %u%%  saved", (unsigned)rgb_pct);
        lv_label_set_text(rgb_lbl, buf);
    }
    lv_obj_set_style_text_color(rgb_lbl, lv_color_hex(0xc4ddd0), 0);

    lv_obj_t *rgb_sl = lv_slider_create(box);
    lv_obj_set_width(rgb_sl, lv_pct(100));
    lv_slider_set_range(rgb_sl, 5, 100);
    lv_slider_set_value(rgb_sl, (int32_t)rgb_pct, LV_ANIM_OFF);
    if (!rgb_on) {
        lv_obj_add_state(rgb_sl, LV_STATE_DISABLED);
    }

    s_rear_ui.sw = rear_sw;
    s_rear_ui.sl = rgb_sl;
    s_rear_ui.lbl_pct = rgb_lbl;
    lv_obj_add_event_cb(rear_sw, on_rear_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(rgb_sl, on_pct_slider_snap, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(rgb_sl, on_rear_slider_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *aslp_sec = lv_label_create(box);
    lv_label_set_text(aslp_sec, "POWER");
    lv_obj_set_style_text_color(aslp_sec, lv_color_hex(0x7a8f86), 0);
    lv_obj_set_style_text_opa(aslp_sec, LV_OPA_90, 0);

    lv_obj_t *aslp_title = lv_label_create(box);
    lv_label_set_text(aslp_title, LV_SYMBOL_POWER "  Idle dim and sleep");
    lv_obj_set_style_text_color(aslp_title, lv_color_hex(0xc4ddd0), 0);

    lv_obj_t *aslp_hint = lv_label_create(box);
    lv_label_set_text(aslp_hint, "Lowers LCD after quiet time, then sleeps. Applies on home and idle karaoke.");
    lv_obj_set_style_text_color(aslp_hint, lv_color_hex(0x5c6e66), 0);
    lv_label_set_long_mode(aslp_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(aslp_hint, lv_pct(100));

    lv_obj_t *aslp_row = lv_obj_create(box);
    lv_obj_set_width(aslp_row, lv_pct(100));
    lv_obj_set_height(aslp_row, 40);
    lv_obj_set_style_pad_all(aslp_row, 0, 0);
    lv_obj_set_style_border_width(aslp_row, 0, 0);
    lv_obj_set_style_bg_opa(aslp_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(aslp_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(aslp_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(aslp_row, 12, 0);
    ui_no_scroll(aslp_row);

    lv_obj_t *aslp_sw = lv_switch_create(aslp_row);
    lv_obj_set_size(aslp_sw, 52, 26);
    if (aslp_on) {
        lv_obj_add_state(aslp_sw, LV_STATE_CHECKED);
    }
    lv_obj_t *aslp_sw_lbl = lv_label_create(aslp_row);
    lv_label_set_text(aslp_sw_lbl, "Enable");
    lv_obj_set_style_text_color(aslp_sw_lbl, lv_color_hex(0xb8d4c8), 0);

    lv_obj_t *aslp_lbl = lv_label_create(box);
    lv_label_set_text(aslp_lbl,
                      aslp_on ? LV_SYMBOL_POWER "  Idle dim + sleep on  saved"
                              : LV_SYMBOL_POWER "  Idle dim + sleep off  saved");
    lv_obj_set_style_text_color(aslp_lbl, lv_color_hex(0xc4ddd0), 0);

    s_aslp_ui.sw = aslp_sw;
    s_aslp_ui.lbl = aslp_lbl;
    lv_obj_add_event_cb(aslp_sw, on_auto_sleep_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_update_layout(outer);
    lv_obj_invalidate(scr);
    lvgl_port_unlock();

    dashcdg_platform_hw_set_screen(DASHCDG_HW_SCREEN_DISPLAY);
    return ESP_OK;
}
