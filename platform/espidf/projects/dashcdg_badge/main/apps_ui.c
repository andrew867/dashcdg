/*
 * Applications hub from home launcher (replaces SD-card placeholder).
 */
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "apps_ui.h"
#include "audio_lab_ui.h"
#include "badge_ui_flair.h"
#include "display_lvgl.h"
#include "home_ui.h"
#include "nav.h"
#include "platform_hw.h"

static const char *TAG = "apps_ui";

#define DASHCDG_APPS_TILE_H 84

static void on_back(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_nav_home(disp);
    }
}

static void on_audio_lab(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_audio_lab_ui_set_return_to_apps_menu(true);
        dashcdg_nav_audio_lab(disp);
    }
}

static lv_obj_t *apps_make_tile(lv_obj_t *parent, const char *title, const char *flair, lv_event_cb_t cb, lv_disp_t *disp)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_height(b, DASHCDG_APPS_TILE_H);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x060c0a), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x00aa55), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_left(b, 10, 0);
    lv_obj_set_style_pad_right(b, 8, 0);
    lv_obj_set_style_pad_top(b, 8, 0);
    lv_obj_set_style_pad_bottom(b, 8, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(b, 4, 0);

    lv_obj_t *t = lv_label_create(b);
    lv_label_set_text(t, title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(t, lv_pct(94));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x77eeaa), 0);

    lv_obj_t *f = lv_label_create(b);
    lv_label_set_text(f, flair ? flair : "");
    lv_label_set_long_mode(f, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(f, lv_pct(94));
    lv_obj_set_style_text_align(f, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(f, lv_color_hex(0x558877), 0);
    lv_obj_set_style_text_opa(f, LV_OPA_90, 0);

    if (cb != NULL) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, disp);
    } else {
        lv_obj_add_state(b, LV_STATE_DISABLED);
    }
    return b;
}

esp_err_t dashcdg_applications_ui_present(lv_disp_t *disp)
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
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(outer, lv_color_hex(0x020403), 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(outer, 8, 0);
    lv_obj_remove_flag(outer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = lv_obj_create(outer);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_height(top, 40);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *b_back = lv_button_create(top);
    lv_obj_set_width(b_back, 88);
    lv_obj_t *lb = lv_label_create(b_back);
    lv_label_set_text(lb, LV_SYMBOL_HOME "  main");
    lv_obj_center(lb);
    lv_obj_add_event_cb(b_back, on_back, LV_EVENT_CLICKED, disp);

    lv_obj_t *top_sp = lv_obj_create(top);
    lv_obj_set_flex_grow(top_sp, 1);
    lv_obj_set_height(top_sp, 1);
    lv_obj_set_style_bg_opa(top_sp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_sp, 0, 0);
    lv_obj_remove_flag(top_sp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(top_sp, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hdr_r = lv_label_create(top);
    lv_label_set_text(hdr_r, LV_SYMBOL_LIST "  apps");
    lv_obj_set_style_text_color(hdr_r, lv_color_hex(0x33ff99), 0);

    lv_obj_t *scroll = lv_obj_create(outer);
    lv_obj_set_width(scroll, lv_pct(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_style_pad_all(scroll, 2, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(scroll, 10, 0);

    lv_obj_t *sub = lv_label_create(scroll);
    lv_label_set_text(sub, dashcdg_ui_flair_movie_tagline());
    lv_obj_set_width(sub, lv_pct(98));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x557766), 0);

    apps_make_tile(scroll, LV_SYMBOL_AUDIO "  Audio lab",
                    "PWM lab, NVS beep level, CDG heap note — same as setup hub, shortcut here.", on_audio_lab, disp);

    apps_make_tile(scroll, LV_SYMBOL_DRIVE "  SD / files",
                    "Reserved — on-device loader when storage path is ready.", NULL, disp);

    lv_obj_update_layout(outer);
    lv_obj_invalidate(scr);
    lvgl_port_unlock();

    dashcdg_platform_hw_set_screen(DASHCDG_HW_SCREEN_APPLICATIONS);
    return ESP_OK;
}
