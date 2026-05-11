#include "karaoke_settings_ui.h"

#include <stdio.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "badge_prefs.h"
#include "badge_rx.h"
#include "karaoke_ui.h"
#include "dashcdg/media_clock.h"
#include "display_lvgl.h"
#include "home_ui.h"
#include "nav.h"
#include "platform_hw.h"
#include "wifi_touch_ui.h"

static const char *TAG = "karaoke_settings_ui";

static lv_timer_t *s_kov_poll_timer;
static lv_obj_t *s_kov_settings_pct_lbl;

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

static void apply_decode_toggles(void)
{
    bool v_on = true;
    bool a_on = true;
    uint8_t pv = 1U;
    uint8_t pa = 1U;

    (void)dashcdg_badge_prefs_load_karaoke_video_decode(&pv);
    (void)dashcdg_badge_prefs_load_karaoke_audio_decode(&pa);
    if (pv == 0U) {
        v_on = false;
    }
    if (pa == 0U) {
        a_on = false;
    }
    dashcdg_badge_rx_set_decode_enabled(v_on, a_on);
    dashcdg_karaoke_ui_sync_decode_layout_from_prefs();
}

static void on_video_decode_sw(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dashcdg_platform_hw_notify_activity();
    (void)dashcdg_badge_prefs_save_karaoke_video_decode(on ? 1U : 0U);
    apply_decode_toggles();
}

static void on_audio_decode_sw(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dashcdg_platform_hw_notify_activity();
    (void)dashcdg_badge_prefs_save_karaoke_audio_decode(on ? 1U : 0U);
    apply_decode_toggles();
}

static void kov_poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    dashcdg_badge_prefs_poll_karaoke_output_volume_save(dashcdg_clock_now_ms());
}

static void on_karaoke_output_vol_slider(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(slider);

    if (v < 0) {
        v = 0;
    }
    if (v > 100) {
        v = 100;
    }
    {
        uint8_t pct = (uint8_t)v;

        dashcdg_platform_hw_notify_activity();
        dashcdg_platform_hw_set_karaoke_output_volume_pct(pct);
        dashcdg_badge_prefs_schedule_karaoke_output_volume_save(pct, dashcdg_clock_now_ms());
        if (s_kov_settings_pct_lbl != NULL && lv_obj_is_valid(s_kov_settings_pct_lbl)) {
            char b[12];

            snprintf(b, sizeof(b), "%u%%", (unsigned)pct);
            lv_label_set_text(s_kov_settings_pct_lbl, b);
        }
    }
}

#if defined(CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK) && CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK
static void on_repair_nack_sw(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dashcdg_platform_hw_notify_activity();
    (void)dashcdg_badge_prefs_save_karaoke_repair_nack(on ? 1U : 0U);
    dashcdg_badge_rx_apply_rx_tuning_prefs();
}
#endif

static void on_v4_stats_tx_sw(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dashcdg_platform_hw_notify_activity();
    (void)dashcdg_badge_prefs_save_karaoke_v4_stats_tx(on ? 1U : 0U);
    dashcdg_badge_rx_apply_rx_tuning_prefs();
}

static void on_media_path_dd(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    uint8_t mode = 3U;

    dashcdg_platform_hw_notify_activity();
    switch (sel) {
    case 0:
        mode = 3U; /* auto */
        break;
    case 1:
        mode = 0U; /* both */
        break;
    case 2:
        mode = 1U; /* mcast prefer */
        break;
    case 3:
        mode = 2U; /* ucast prefer */
        break;
    default:
        mode = 3U;
        break;
    }
    (void)dashcdg_badge_prefs_save_karaoke_media_path_policy(mode);
    dashcdg_badge_rx_apply_rx_tuning_prefs();
}

esp_err_t dashcdg_karaoke_settings_ui_present(lv_disp_t *disp)
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
    lv_label_set_text(title, LV_SYMBOL_VIDEO "  Karaoke decode");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe8f0ec), 0);

    lv_obj_t *hint = lv_label_create(scroll);
    lv_label_set_text(hint, "Decode off isolates Wi-Fi + clock. RX tuning: repair NACKs need TX FEC; stats uplink feeds desktop HUD.");
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

    uint8_t pref_video = 1U;
    uint8_t pref_audio = 1U;
    uint8_t pref_stx = 1U;
    uint8_t pref_mpath = 3U;
#if defined(CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK) && CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK
    uint8_t pref_nack = 1U;
#endif
    (void)dashcdg_badge_prefs_load_karaoke_video_decode(&pref_video);
    (void)dashcdg_badge_prefs_load_karaoke_audio_decode(&pref_audio);
#if defined(CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK) && CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK
    (void)dashcdg_badge_prefs_load_karaoke_repair_nack(&pref_nack);
#endif
    (void)dashcdg_badge_prefs_load_karaoke_v4_stats_tx(&pref_stx);
    (void)dashcdg_badge_prefs_load_karaoke_media_path_policy(&pref_mpath);

    lv_obj_t *video_row = lv_obj_create(box);
    lv_obj_set_width(video_row, lv_pct(100));
    lv_obj_set_height(video_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(video_row, 0, 0);
    lv_obj_set_style_border_width(video_row, 0, 0);
    lv_obj_set_style_bg_opa(video_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(video_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(video_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_no_scroll(video_row);

    lv_obj_t *video_lbl = lv_label_create(video_row);
    lv_label_set_text(video_lbl, "Video decode");
    lv_obj_set_style_text_color(video_lbl, lv_color_hex(0xaabbcc), 0);

    lv_obj_t *video_sw = lv_switch_create(video_row);
    if (pref_video != 0U) {
        lv_obj_add_state(video_sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(video_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(video_sw, on_video_decode_sw, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *audio_row = lv_obj_create(box);
    lv_obj_set_width(audio_row, lv_pct(100));
    lv_obj_set_height(audio_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(audio_row, 0, 0);
    lv_obj_set_style_border_width(audio_row, 0, 0);
    lv_obj_set_style_bg_opa(audio_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(audio_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(audio_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_no_scroll(audio_row);

    lv_obj_t *audio_lbl = lv_label_create(audio_row);
    lv_label_set_text(audio_lbl, "Audio decode");
    lv_obj_set_style_text_color(audio_lbl, lv_color_hex(0xaabbcc), 0);

    lv_obj_t *audio_sw = lv_switch_create(audio_row);
    if (pref_audio != 0U) {
        lv_obj_add_state(audio_sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(audio_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(audio_sw, on_audio_decode_sw, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *kov_row = lv_obj_create(box);
    lv_obj_set_width(kov_row, lv_pct(100));
    lv_obj_set_height(kov_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(kov_row, 0, 0);
    lv_obj_set_style_border_width(kov_row, 0, 0);
    lv_obj_set_style_bg_opa(kov_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(kov_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(kov_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(kov_row, 10, 0);
    ui_no_scroll(kov_row);

    lv_obj_t *kov_lbl = lv_label_create(kov_row);
    lv_label_set_text(kov_lbl, "Output volume");
    lv_obj_set_style_text_color(kov_lbl, lv_color_hex(0xaabbcc), 0);

    lv_obj_t *kov_sl = lv_slider_create(kov_row);
    lv_obj_set_flex_grow(kov_sl, 1);
    lv_slider_set_range(kov_sl, 0, 100);
    lv_slider_set_value(kov_sl, (int32_t)dashcdg_platform_hw_get_karaoke_output_volume_pct(), LV_ANIM_OFF);
    lv_obj_add_event_cb(kov_sl, on_karaoke_output_vol_slider, LV_EVENT_VALUE_CHANGED, NULL);

    s_kov_settings_pct_lbl = lv_label_create(kov_row);
    {
        char pctb[12];
        snprintf(pctb, sizeof(pctb), "%u%%", (unsigned)dashcdg_platform_hw_get_karaoke_output_volume_pct());
        lv_label_set_text(s_kov_settings_pct_lbl, pctb);
    }
    lv_obj_set_style_text_color(s_kov_settings_pct_lbl, lv_color_hex(0x88ddbb), 0);
    lv_obj_set_style_min_width(s_kov_settings_pct_lbl, 40, 0);

    lv_obj_t *kov_hint = lv_label_create(box);
    lv_label_set_text(kov_hint, "0% = amp off (quiet). Saved ~5s after you stop dragging.");
    lv_obj_set_width(kov_hint, lv_pct(100));
    lv_label_set_long_mode(kov_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(kov_hint, lv_color_hex(0x5a6d68), 0);

    lv_obj_t *tune_title = lv_label_create(scroll);
    lv_label_set_text(tune_title, LV_SYMBOL_WIFI "  RX tuning (reliability)");
    lv_obj_set_style_text_color(tune_title, lv_color_hex(0xe8f0ec), 0);

    lv_obj_t *tune_box = lv_obj_create(scroll);
    lv_obj_set_width(tune_box, lv_pct(100));
    lv_obj_set_height(tune_box, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(tune_box, 12, 0);
    lv_obj_set_style_border_width(tune_box, 1, 0);
    lv_obj_set_style_border_color(tune_box, lv_color_hex(0x1e3a30), 0);
    lv_obj_set_style_bg_color(tune_box, lv_color_hex(0x0a1012), 0);
    lv_obj_set_style_bg_opa(tune_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tune_box, 10, 0);
    lv_obj_set_flex_flow(tune_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tune_box, 12, 0);
    ui_no_scroll(tune_box);

#if defined(CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK) && CONFIG_DASHCDG_BADGE_ENABLE_REPAIR_NACK
    lv_obj_t *nack_row = lv_obj_create(tune_box);
    lv_obj_set_width(nack_row, lv_pct(100));
    lv_obj_set_height(nack_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(nack_row, 0, 0);
    lv_obj_set_style_border_width(nack_row, 0, 0);
    lv_obj_set_style_bg_opa(nack_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(nack_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nack_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_no_scroll(nack_row);
    lv_obj_t *nack_lbl = lv_label_create(nack_row);
    lv_label_set_text(nack_lbl, "CDG repair NACK");
    lv_obj_set_style_text_color(nack_lbl, lv_color_hex(0xaabbcc), 0);
    lv_obj_t *nack_sw = lv_switch_create(nack_row);
    if (pref_nack != 0U) {
        lv_obj_add_state(nack_sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(nack_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(nack_sw, on_repair_nack_sw, LV_EVENT_VALUE_CHANGED, NULL);
#endif

    lv_obj_t *stx_row = lv_obj_create(tune_box);
    lv_obj_set_width(stx_row, lv_pct(100));
    lv_obj_set_height(stx_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(stx_row, 0, 0);
    lv_obj_set_style_border_width(stx_row, 0, 0);
    lv_obj_set_style_bg_opa(stx_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(stx_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stx_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_no_scroll(stx_row);
    lv_obj_t *stx_lbl = lv_label_create(stx_row);
    lv_label_set_text(stx_lbl, "TX stats uplink");
    lv_obj_set_style_text_color(stx_lbl, lv_color_hex(0xaabbcc), 0);
    lv_obj_t *stx_sw = lv_switch_create(stx_row);
    if (pref_stx != 0U) {
        lv_obj_add_state(stx_sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(stx_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(stx_sw, on_v4_stats_tx_sw, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *mp_row = lv_obj_create(tune_box);
    lv_obj_set_width(mp_row, lv_pct(100));
    lv_obj_set_height(mp_row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(mp_row, 0, 0);
    lv_obj_set_style_border_width(mp_row, 0, 0);
    lv_obj_set_style_bg_opa(mp_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(mp_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mp_row, 6, 0);
    ui_no_scroll(mp_row);

    lv_obj_t *mp_lbl = lv_label_create(mp_row);
    lv_label_set_text(mp_lbl, "OpenWrt media path");
    lv_obj_set_style_text_color(mp_lbl, lv_color_hex(0xaabbcc), 0);

    lv_obj_t *mp_dd = lv_dropdown_create(mp_row);
    lv_obj_set_width(mp_dd, lv_pct(100));
    lv_dropdown_set_options(mp_dd, "auto\nboth\nmcast prefer\nucast prefer");
    if (pref_mpath == 0U) {
        lv_dropdown_set_selected(mp_dd, 1);
    } else if (pref_mpath == 1U) {
        lv_dropdown_set_selected(mp_dd, 2);
    } else if (pref_mpath == 2U) {
        lv_dropdown_set_selected(mp_dd, 3);
    } else {
        lv_dropdown_set_selected(mp_dd, 0);
    }
    lv_obj_add_event_cb(mp_dd, on_media_path_dd, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_update_layout(outer);
    lv_obj_invalidate(scr);

    if (s_kov_poll_timer == NULL) {
        s_kov_poll_timer = lv_timer_create(kov_poll_timer_cb, 300, NULL);
    }

    lvgl_port_unlock();

    apply_decode_toggles();
    dashcdg_badge_rx_apply_rx_tuning_prefs();
    dashcdg_platform_hw_set_screen(DASHCDG_HW_SCREEN_SETTINGS);
    return ESP_OK;
}
