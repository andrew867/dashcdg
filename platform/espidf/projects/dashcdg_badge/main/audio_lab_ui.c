/*
 * Audio lab: touch beep + PWM level (NVS), karaoke-style shell, YM-ish demo with play/pause on IO26.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "esp_wifi.h"
#include "lvgl.h"

#include "audio_lab_ui.h"
#include "badge_lab_ym.h"
#include "badge_prefs.h"
#include "display_lvgl.h"
#include "home_ui.h"
#include "nav.h"
#include "platform_hw.h"
#include "vbat_sense.h"
#include "wifi_touch_ui.h"

static const char *TAG = "audio_lab_ui";

#define AUDIO_LAB_STATUS_SLOW_MS 2500U

static bool s_audio_lab_pending_return_to_apps;
static bool s_audio_lab_this_screen_returns_to_apps;

static lv_timer_t *s_status_timer;
static lv_obj_t *s_bar_wifi;
static lv_obj_t *s_bar_bat;
static lv_obj_t *s_bar_line;
static lv_obj_t *s_lab_modal_root;
static lv_obj_t *s_play_lbl;
static bool s_ym_playing;

void dashcdg_audio_lab_ui_set_return_to_apps_menu(bool return_to_apps)
{
    s_audio_lab_pending_return_to_apps = return_to_apps;
}

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

static void ui_no_scroll(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static lv_color_t lab_bat_color_from_pack_mv(int vbat_mv)
{
    const int mv_red = 3450;
    const int mv_green = 4180;
    int x = (vbat_mv - mv_red) * 255 / (mv_green - mv_red);
    if (x < 0) {
        x = 0;
    }
    if (x > 255) {
        x = 255;
    }
    return lv_color_mix(lv_color_hex(0x33dd66), lv_color_hex(0xff4444), (uint8_t)x);
}

static const char *lab_bat_sym_from_mv(int vbat_mv)
{
    if (vbat_mv >= 4180) {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (vbat_mv >= 4000) {
        return LV_SYMBOL_BATTERY_3;
    }
    if (vbat_mv >= 3850) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (vbat_mv >= 3650) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void lab_status_fill_bat(char *bat_txt, size_t bat_sz, lv_color_t *bat_col)
{
    int raw = 0;
    int vbat = 0;
    esp_err_t br = ESP_FAIL;
    if (dashcdg_platform_hw_is_ready()) {
        br = dashcdg_platform_hw_battery_read(&raw, NULL, &vbat);
    }
    if (br != ESP_OK) {
        if (!dashcdg_vbat_sense_is_ready() || dashcdg_vbat_sense_read(&raw, NULL, &vbat) != ESP_OK) {
            snprintf(bat_txt, bat_sz, LV_SYMBOL_BATTERY_EMPTY " --");
            *bat_col = lv_color_hex(0x887766);
            return;
        }
    }
    int deci = (vbat * 10 + 500) / 1000;
    if (deci < 0) {
        deci = 0;
    }
    int w = deci / 10;
    int f = deci % 10;
    (void)raw;
    snprintf(bat_txt, bat_sz, "%s %d.%dV", lab_bat_sym_from_mv(vbat), w, f);
    *bat_col = lab_bat_color_from_pack_mv(vbat);
}

static void lab_status_fill_wifi(char *wifi_txt, size_t wifi_sz)
{
    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));
    esp_err_t werr = esp_wifi_sta_get_ap_info(&ap);
    if (werr == ESP_ERR_WIFI_NOT_INIT) {
        snprintf(wifi_txt, wifi_sz, LV_SYMBOL_WIFI " off");
    } else if (werr == ESP_OK) {
        snprintf(wifi_txt, wifi_sz, LV_SYMBOL_WIFI " %d", ap.rssi);
    } else {
        snprintf(wifi_txt, wifi_sz, LV_SYMBOL_WIFI " --");
    }
}

static bool lab_status_widgets_ok(void)
{
    return s_bar_wifi && lv_obj_is_valid(s_bar_wifi) && s_bar_bat && lv_obj_is_valid(s_bar_bat) && s_bar_line &&
           lv_obj_is_valid(s_bar_line);
}

static void lab_status_bar_update_slow(void)
{
    if (!lab_status_widgets_ok()) {
        return;
    }
    char wbuf[40];
    lab_status_fill_wifi(wbuf, sizeof(wbuf));
    lv_label_set_text(s_bar_wifi, wbuf);

    char bbuf[40];
    lv_color_t bcol;
    lab_status_fill_bat(bbuf, sizeof(bbuf), &bcol);
    lv_label_set_text(s_bar_bat, bbuf);
    lv_obj_set_style_text_color(s_bar_bat, bcol, 0);
}

static void lab_status_line_set(void)
{
    if (!s_bar_line || !lv_obj_is_valid(s_bar_line)) {
        return;
    }
    if (s_ym_playing) {
        lv_label_set_text(s_bar_line, LV_SYMBOL_AUDIO "  YM demo ~8 kHz");
        lv_obj_set_style_text_color(s_bar_line, lv_color_hex(0x66ddaa), 0);
    } else {
        lv_label_set_text(s_bar_line, LV_SYMBOL_PAUSE "  idle");
        lv_obj_set_style_text_color(s_bar_line, lv_color_hex(0x998866), 0);
    }
}

static void on_status_timer(lv_timer_t *t)
{
    (void)t;
    if (!lvgl_port_lock(50)) {
        return;
    }
    lab_status_bar_update_slow();
    lvgl_port_unlock();
}

static void lab_modal_close(void)
{
    if (s_lab_modal_root && lv_obj_is_valid(s_lab_modal_root)) {
        lv_obj_del(s_lab_modal_root);
    }
    s_lab_modal_root = NULL;
}

static bool lab_modal_is_open(void)
{
    return s_lab_modal_root != NULL && lv_obj_is_valid(s_lab_modal_root);
}

static void on_modal_scrim(lv_event_t *e)
{
    (void)e;
    lab_modal_close();
}

static void on_modal_panel(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

static void on_modal_ok(lv_event_t *e)
{
    (void)e;
    lab_modal_close();
}

static void on_info_btn(lv_event_t *e)
{
    (void)e;
    if (lab_modal_is_open()) {
        return;
    }

    lv_obj_t *layer = lv_layer_top();
    lv_obj_t *root = lv_obj_create(layer);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(0x080a09), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_add_event_cb(root, on_modal_scrim, LV_EVENT_CLICKED, NULL);
    ui_no_scroll(root);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    s_lab_modal_root = root;

    lv_obj_t *panel = lv_obj_create(root);
    lv_obj_set_width(panel, lv_pct(90));
    lv_obj_set_height(panel, lv_pct(72));
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05080a), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x00aa88), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_add_event_cb(panel, on_modal_panel, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 6, 0);
    ui_no_scroll(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mtitle = lv_label_create(panel);
    lv_label_set_text(mtitle, "[ AUDIO LAB ]");
    lv_obj_set_style_text_color(mtitle, lv_color_hex(0x66ffcc), 0);

    lv_obj_t *scroll = lv_obj_create(panel);
    lv_obj_set_width(scroll, lv_pct(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_style_min_height(scroll, 120, 0);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(scroll, 4, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(scroll, on_modal_panel, LV_EVENT_CLICKED, NULL);

    lv_obj_t *body = lv_label_create(scroll);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, lv_color_hex(0xaaccbb), 0);
    lv_label_set_text(body,
                      "IO26 is LEDC PWM into the SC8002B speaker path (not I2S). Touch beep and the "
                      "volume slider set NVS prefs shared with the rest of the UI.\n\n"
                      "Play runs a tiny in-tree square-wave arpeggio (~8 kHz) as duty PCM on a ~24 kHz carrier "
                      "for a rough chiptune check — keep volume moderate on a small speaker.");

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 32);
    lv_obj_set_flex_grow(row, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_width(b, 72);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0a1510), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x338866), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, "> ok");
    lv_obj_set_style_text_color(bl, lv_color_hex(0x66ffcc), 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, on_modal_ok, LV_EVENT_CLICKED, NULL);

    lv_obj_update_layout(panel);
    lv_obj_move_foreground(root);
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
        snprintf(buf, sizeof(buf), LV_SYMBOL_AUDIO "  %d%%  saved", v);
        lv_label_set_text(lbl, buf);
    }
}

static void on_touch_beep_changed(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    uint8_t onu = on ? 1U : 0U;
    dashcdg_platform_hw_notify_activity();
    dashcdg_platform_hw_set_touch_beep_enabled(on);
    (void)dashcdg_badge_prefs_save_touch_beep(onu);
}

static void on_play_pause(lv_event_t *e)
{
    (void)e;
    dashcdg_platform_hw_notify_activity();
    s_ym_playing = !s_ym_playing;
    dashcdg_badge_lab_ym_play_set(s_ym_playing);
    if (s_play_lbl && lv_obj_is_valid(s_play_lbl)) {
        lv_label_set_text(s_play_lbl, s_ym_playing ? LV_SYMBOL_PAUSE "  Pause" : LV_SYMBOL_PLAY "  Play");
    }
    lab_status_line_set();
}

static void on_back(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    s_ym_playing = false;
    dashcdg_badge_lab_ym_stop();
    lab_modal_close();
    if (s_status_timer) {
        lv_timer_del(s_status_timer);
        s_status_timer = NULL;
    }
    s_bar_wifi = NULL;
    s_bar_bat = NULL;
    s_bar_line = NULL;
    if (disp) {
        if (s_audio_lab_this_screen_returns_to_apps) {
            dashcdg_nav_applications(disp);
        } else {
            dashcdg_nav_settings(disp);
        }
    }
}

esp_err_t dashcdg_audio_lab_ui_present(lv_disp_t *disp)
{
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_ARG, TAG, "disp");

    dashcdg_badge_lab_ym_stop();
    s_ym_playing = false;

    s_audio_lab_this_screen_returns_to_apps = s_audio_lab_pending_return_to_apps;
    s_audio_lab_pending_return_to_apps = false;

    dashcdg_home_ui_pause_status_updates();
    dashcdg_wifi_drop_lvgl_refs();

    if (s_status_timer) {
        lv_timer_del(s_status_timer);
        s_status_timer = NULL;
    }
    s_bar_wifi = NULL;
    s_bar_bat = NULL;
    s_bar_line = NULL;
    s_play_lbl = NULL;
    lab_modal_close();

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    dashcdg_display_clear_top_layer(disp);

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x020204), 0);
    ui_no_scroll(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *outer = lv_obj_create(scr);
    lv_obj_set_size(outer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_hor(outer, 6, 0);
    lv_obj_set_style_pad_top(outer, 0, 0);
    lv_obj_set_style_pad_bottom(outer, 0, 0);
    lv_obj_set_style_border_width(outer, 0, 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(outer, 0, 0);
    ui_no_scroll(outer);

    lv_obj_t *top = lv_obj_create(outer);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_height(top, 22);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_no_scroll(top);

    lv_obj_t *b_back = lv_button_create(top);
    lv_obj_set_size(b_back, 26, 22);
    lv_obj_set_style_pad_all(b_back, 0, 0);
    lv_obj_set_style_radius(b_back, 3, 0);
    lv_obj_set_style_bg_color(b_back, lv_color_hex(0x0a1510), 0);
    lv_obj_set_style_border_color(b_back, lv_color_hex(0x338866), 0);
    lv_obj_set_style_border_width(b_back, 1, 0);
    lv_obj_set_style_shadow_width(b_back, 0, 0);
    lv_obj_t *lb = lv_label_create(b_back);
    lv_label_set_text(lb, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(lb, lv_color_hex(0x66ffcc), 0);
    lv_obj_center(lb);
    lv_obj_add_event_cb(b_back, on_back, LV_EVENT_CLICKED, disp);

    lv_obj_t *row_stat = lv_obj_create(top);
    lv_obj_set_height(row_stat, 22);
    lv_obj_set_flex_grow(row_stat, 1);
    lv_obj_set_style_min_width(row_stat, 0, 0);
    lv_obj_set_style_pad_all(row_stat, 0, 0);
    lv_obj_set_style_border_width(row_stat, 0, 0);
    lv_obj_set_style_bg_opa(row_stat, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row_stat, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_stat, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row_stat, 3, 0);
    ui_no_scroll(row_stat);

    s_bar_wifi = lv_label_create(row_stat);
    lv_label_set_long_mode(s_bar_wifi, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_color(s_bar_wifi, lv_color_hex(0xaaccdd), 0);
    lv_label_set_text(s_bar_wifi, LV_SYMBOL_WIFI " --");

    s_bar_bat = lv_label_create(row_stat);
    lv_label_set_long_mode(s_bar_bat, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_color(s_bar_bat, lv_color_hex(0xaaccbb), 0);
    lv_label_set_text(s_bar_bat, LV_SYMBOL_BATTERY_EMPTY " --");

    s_bar_line = lv_label_create(row_stat);
    lv_obj_set_flex_grow(s_bar_line, 1);
    lv_obj_set_style_min_width(s_bar_line, 0, 0);
    lv_label_set_long_mode(s_bar_line, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(s_bar_line, LV_TEXT_ALIGN_LEFT, 0);
    lab_status_line_set();

    lv_obj_t *b_info = lv_button_create(top);
    lv_obj_set_size(b_info, 26, 22);
    lv_obj_set_style_pad_all(b_info, 0, 0);
    lv_obj_set_style_radius(b_info, 3, 0);
    lv_obj_set_style_bg_color(b_info, lv_color_hex(0x0a1510), 0);
    lv_obj_set_style_border_color(b_info, lv_color_hex(0x338866), 0);
    lv_obj_set_style_border_width(b_info, 1, 0);
    lv_obj_set_style_shadow_width(b_info, 0, 0);
    lv_obj_t *li = lv_label_create(b_info);
    lv_label_set_text(li, "?");
    lv_obj_set_style_text_color(li, lv_color_hex(0x88ffcc), 0);
    lv_obj_center(li);
    lv_obj_add_event_cb(b_info, on_info_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *stage = lv_obj_create(outer);
    lv_obj_set_width(stage, lv_pct(100));
    lv_obj_set_flex_grow(stage, 1);
    lv_obj_set_style_pad_all(stage, 6, 0);
    lv_obj_set_style_pad_top(stage, 8, 0);
    lv_obj_set_style_border_width(stage, 0, 0);
    lv_obj_set_style_bg_opa(stage, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(stage, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stage, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(stage, 10, 0);
    ui_no_scroll(stage);

    lv_obj_t *hdr = lv_label_create(stage);
    lv_label_set_text(hdr, LV_SYMBOL_AUDIO "  Audio");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x88ddbb), 0);

    uint8_t touch_beep_on = 1;
    (void)dashcdg_badge_prefs_load_touch_beep(&touch_beep_on);
    dashcdg_platform_hw_set_touch_beep_enabled(touch_beep_on != 0);

    lv_obj_t *tb_row = lv_obj_create(stage);
    lv_obj_set_width(tb_row, lv_pct(100));
    lv_obj_set_height(tb_row, 36);
    lv_obj_set_style_pad_all(tb_row, 0, 0);
    lv_obj_set_style_border_width(tb_row, 0, 0);
    lv_obj_set_style_bg_opa(tb_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(tb_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tb_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_no_scroll(tb_row);

    lv_obj_t *tb_sw = lv_switch_create(tb_row);
    lv_obj_set_size(tb_sw, 52, 26);
    if (touch_beep_on) {
        lv_obj_add_state(tb_sw, LV_STATE_CHECKED);
    }
    lv_obj_t *tb_lbl = lv_label_create(tb_row);
    lv_label_set_text(tb_lbl, "Touch beep");
    lv_obj_set_style_text_color(tb_lbl, lv_color_hex(0xb8d4c8), 0);
    lv_obj_add_event_cb(tb_sw, on_touch_beep_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *vol_sec = lv_label_create(stage);
    lv_label_set_text(vol_sec, "BEEP / PWM LEVEL");
    lv_obj_set_style_text_color(vol_sec, lv_color_hex(0x7a8f86), 0);
    lv_obj_set_style_text_opa(vol_sec, LV_OPA_90, 0);

    uint8_t beep_pct = 85;
    (void)dashcdg_badge_prefs_load_beep_volume(&beep_pct);
    beep_pct = (uint8_t)snap_pct_step5((int)beep_pct);
    dashcdg_platform_hw_set_beep_volume_pct(beep_pct);

    lv_obj_t *vol_lbl = lv_label_create(stage);
    {
        char buf[56];
        snprintf(buf, sizeof(buf), LV_SYMBOL_AUDIO "  %u%%  saved", (unsigned)beep_pct);
        lv_label_set_text(vol_lbl, buf);
    }
    lv_obj_set_style_text_color(vol_lbl, lv_color_hex(0xc4ddd0), 0);

    lv_obj_t *vol_sl = lv_slider_create(stage);
    lv_obj_set_width(vol_sl, lv_pct(100));
    lv_slider_set_range(vol_sl, 5, 100);
    lv_slider_set_value(vol_sl, (int32_t)beep_pct, LV_ANIM_OFF);
    lv_obj_add_event_cb(vol_sl, on_pct_slider_snap, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(vol_sl, on_vol_changed, LV_EVENT_VALUE_CHANGED, vol_lbl);

    lv_obj_t *demo_sec = lv_label_create(stage);
    lv_label_set_text(demo_sec, "YM DEMO");
    lv_obj_set_style_text_color(demo_sec, lv_color_hex(0x7a8f86), 0);
    lv_obj_set_style_text_opa(demo_sec, LV_OPA_90, 0);

    lv_obj_t *play_btn = lv_button_create(stage);
    lv_obj_set_width(play_btn, lv_pct(100));
    lv_obj_set_height(play_btn, 44);
    s_play_lbl = lv_label_create(play_btn);
    lv_label_set_text(s_play_lbl, LV_SYMBOL_PLAY "  Play");
    lv_obj_set_style_text_color(s_play_lbl, lv_color_hex(0xccffee), 0);
    lv_obj_center(s_play_lbl);
    lv_obj_add_event_cb(play_btn, on_play_pause, LV_EVENT_CLICKED, NULL);

    lv_obj_t *dock = lv_obj_create(outer);
    lv_obj_set_width(dock, lv_pct(100));
    lv_obj_set_height(dock, 28);
    lv_obj_set_style_pad_hor(dock, 4, 0);
    lv_obj_set_style_pad_ver(dock, 4, 0);
    lv_obj_set_style_bg_color(dock, lv_color_hex(0x030605), 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(dock, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_set_style_border_color(dock, lv_color_hex(0x115533), 0);
    ui_no_scroll(dock);

    lv_obj_t *dock_l = lv_label_create(dock);
    lv_label_set_long_mode(dock_l, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(dock_l, lv_pct(96));
    lv_obj_set_style_text_color(dock_l, lv_color_hex(0x558877), 0);
    lv_label_set_text(dock_l, "PWM IO26  " LV_SYMBOL_VOLUME_MAX " 24 kHz carrier");

    lv_obj_update_layout(outer);
    lv_obj_invalidate(scr);
    lvgl_port_unlock();

    lab_status_bar_update_slow();
    s_status_timer = lv_timer_create(on_status_timer, (uint32_t)AUDIO_LAB_STATUS_SLOW_MS, NULL);
    if (!s_status_timer) {
        ESP_LOGW(TAG, "status lv_timer_create failed");
    }

    dashcdg_platform_hw_set_screen(DASHCDG_HW_SCREEN_AUDIO_LAB);
    return ESP_OK;
}
