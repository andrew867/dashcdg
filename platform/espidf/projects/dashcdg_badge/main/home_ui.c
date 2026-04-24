/*
 * Launcher + status dock. "Terminal phreak" vibe; compact tiles; sysinfo modal.
 */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "lvgl.h"

#include "display_lvgl.h"
#include "home_ui.h"
#include "nav.h"
#include "vbat_sense.h"

/* TODO(remove): on-screen XPT2046 read snapshot (LVGL-mapped x/y + z + PENIRQ). Set 0 before ship. */
#define DASHCDG_HOME_TOUCH_DEBUG_OVERLAY 0

static const char *TAG = "home_ui";

static lv_timer_t *s_status_timer;
#if DASHCDG_HOME_TOUCH_DEBUG_OVERLAY
static lv_timer_t *s_touch_dbg_timer;
static lv_obj_t *s_touch_dbg_lbl;
#endif
static lv_obj_t *s_lbl_wifi;
static lv_obj_t *s_lbl_ip;
static lv_obj_t *s_lbl_bat;
static lv_obj_t *s_info_modal;

/** Battery label text color: dedicated style so theme refresh cannot revert to primary (blue). */
static lv_style_t s_home_bat_text_style;
static bool s_home_bat_text_style_inited;

static void home_bat_text_style_attach(lv_obj_t *lbl_bat)
{
    if (!lbl_bat) {
        return;
    }
    if (!s_home_bat_text_style_inited) {
        lv_style_init(&s_home_bat_text_style);
        lv_style_set_text_color(&s_home_bat_text_style, lv_color_hex(0xccbb66));
        s_home_bat_text_style_inited = true;
    }
    lv_obj_add_style(lbl_bat, &s_home_bat_text_style, 0);
}

/** LVGL 9 defaults lv_obj to scrollable; tiny flex overflows draw scrollbar chrome on the status dock. */
static void dashcdg_ui_no_scroll(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void on_tile_wifi(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_nav_wifi(disp);
    }
}

static void on_tile_karaoke(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_nav_karaoke(disp);
    }
}

static void on_tile_settings(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_nav_settings(disp);
    }
}

static void sysinfo_modal_close(void)
{
    if (s_info_modal && lv_obj_is_valid(s_info_modal)) {
        lv_obj_del(s_info_modal);
    }
    s_info_modal = NULL;
}

static void on_modal_scrim_clicked(lv_event_t *e)
{
    (void)e;
    sysinfo_modal_close();
}

static void on_modal_panel_clicked(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

static void on_modal_ok_click(lv_event_t *e)
{
    (void)e;
    sysinfo_modal_close();
}

static void on_info_btn(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (!disp || s_info_modal) {
        return;
    }

    sysinfo_modal_close();

    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));
    esp_err_t werr = esp_wifi_sta_get_ap_info(&ap);

    char body[720];
    const esp_app_desc_t *app = esp_app_get_description();

    snprintf(body, sizeof(body),
             "> dashcdg_badge payload\n"
             "fw_ver .. %s\n"
             "idf ..... %s\n"
             "proj .... %s\n",
             app->version[0] ? app->version : "?",
             app->idf_ver,
             app->project_name[0] ? app->project_name : "dashcdg");

    if (werr == ESP_ERR_WIFI_NOT_INIT) {
        size_t z0 = strlen(body);
        snprintf(body + z0, sizeof(body) - z0, "\n[wifi] stack not up\n");
    } else if (werr == ESP_OK) {
        char ip_s[20] = "--";
        char gw_s[20] = "--";
        char nm_s[20] = "--";
        esp_netif_t *na = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (na) {
            esp_netif_ip_info_t ipi;
            if (esp_netif_get_ip_info(na, &ipi) == ESP_OK) {
                snprintf(ip_s, sizeof(ip_s), IPSTR, IP2STR(&ipi.ip));
                snprintf(gw_s, sizeof(gw_s), IPSTR, IP2STR(&ipi.gw));
                snprintf(nm_s, sizeof(nm_s), IPSTR, IP2STR(&ipi.netmask));
            }
        }
        char bssid_s[24] = "??:??:??";
        if (ap.bssid[0] || ap.bssid[1]) {
            snprintf(bssid_s, sizeof(bssid_s), "%02x:%02x:%02x:%02x:%02x:%02x", ap.bssid[0], ap.bssid[1],
                     ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        }
        size_t z = strlen(body);
        snprintf(body + z, sizeof(body) - z,
                 "\n--- STA dump ---\n"
                 "ssid .... %s\n"
                 "bssid ... %s\n"
                 "ch ...... %u\n"
                 "rssi .... %d dBm\n"
                 "ip ...... %s\n"
                 "gw ...... %s\n"
                 "mask .... %s\n",
                 (const char *)ap.ssid, bssid_s, (unsigned)ap.primary, ap.rssi, ip_s, gw_s, nm_s);
    } else {
        size_t z1 = strlen(body);
        snprintf(body + z1, sizeof(body) - z1, "\n[wifi] idle / disconnected\n");
    }

    lv_obj_t *layer = lv_layer_top();
    s_info_modal = lv_obj_create(layer);
    lv_obj_set_size(s_info_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_info_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_info_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_info_modal, 0, 0);
    lv_obj_add_event_cb(s_info_modal, on_modal_scrim_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(s_info_modal);
    lv_obj_set_width(panel, 300);
    lv_obj_set_style_max_height(panel, 220, 0);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05080a), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x00cc66), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_shadow_width(panel, 12, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(0x003322), 0);
    lv_obj_add_event_cb(panel, on_modal_panel_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "[ SYS | INF0 ]");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00ff99), 0);

    lv_obj_t *txt = lv_label_create(panel);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(txt, lv_pct(100));
    lv_obj_set_style_text_color(txt, lv_color_hex(0x88ccb0), 0);
    lv_label_set_text(txt, body);

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 32);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_width(b, 72);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x113322), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0x00aa55), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, "> ok");
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, on_modal_ok_click, LV_EVENT_CLICKED, NULL);

    lv_obj_update_layout(panel);
}

void dashcdg_home_ui_pause_status_updates(void)
{
    if (s_status_timer) {
        lv_timer_del(s_status_timer);
        s_status_timer = NULL;
    }
#if DASHCDG_HOME_TOUCH_DEBUG_OVERLAY
    if (s_touch_dbg_timer) {
        lv_timer_del(s_touch_dbg_timer);
        s_touch_dbg_timer = NULL;
    }
    s_touch_dbg_lbl = NULL;
#endif
    s_lbl_wifi = NULL;
    s_lbl_ip = NULL;
    s_lbl_bat = NULL;
    sysinfo_modal_close();
}

/*
 * Pack-side colour: red toward ~3.45 V (3.3 V rail + ME6217-class dropout headroom), green toward
 * ~4.2 V full; smooth mix in between (ESP brownout is on the regulated rail, not this sense node).
 */
static lv_color_t home_bat_color_from_pack_mv(int vbat_mv)
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
    /* lv_color_mix(c1,c2,mix): mix→255 pulls toward c1, mix→0 toward c2 — green high, red low. */
    return lv_color_mix(lv_color_hex(0x33dd66), lv_color_hex(0xff4444), (uint8_t)x);
}

static void home_status_fill_bat(char *bat_txt, size_t bat_sz, lv_color_t *bat_col)
{
    int raw = 0;
    int vbat = 0;
    if (!dashcdg_vbat_sense_is_ready() || dashcdg_vbat_sense_read(&raw, NULL, &vbat) != ESP_OK) {
        snprintf(bat_txt, bat_sz, " -- ");
        *bat_col = lv_color_hex(0x887766);
        return;
    }
    int deci = (vbat * 10 + 500) / 1000;
    if (deci < 0) {
        deci = 0;
    }
    int w = deci / 10;
    int f = deci % 10;
    snprintf(bat_txt, bat_sz, "%4d %d.%dV", raw, w, f);
    *bat_col = home_bat_color_from_pack_mv(vbat);
}

#if DASHCDG_HOME_TOUCH_DEBUG_OVERLAY
static void home_touch_dbg_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_touch_dbg_lbl || !lv_obj_is_valid(s_touch_dbg_lbl)) {
        return;
    }
    char line[120];
    dashcdg_touch_debug_format_line(line, sizeof(line));
    lv_label_set_text(s_touch_dbg_lbl, line);
}
#endif

/*
 * LVGL timers run on the LVGL task — never lvgl_port_lock from timer cb.
 */
static void home_status_gather(char *wifi_txt, size_t wifi_sz, char *ip_txt, size_t ip_sz, char *bat_txt, size_t bat_sz,
                               lv_color_t *bat_col)
{
    home_status_fill_bat(bat_txt, bat_sz, bat_col);

    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));

    esp_err_t werr = esp_wifi_sta_get_ap_info(&ap);
    if (werr == ESP_ERR_WIFI_NOT_INIT) {
        snprintf(wifi_txt, wifi_sz, "[wifi] off");
        snprintf(ip_txt, ip_sz, "--.--.--.--");
    } else if (werr == ESP_OK) {
        const char *ssid = (const char *)ap.ssid;
        /* One line: truncate SSID hard so the bar never wraps on long names */
        if (ssid[0]) {
            if (strlen(ssid) > 10) {
                snprintf(wifi_txt, wifi_sz, "%.8s~ %ddBm", ssid, ap.rssi);
            } else {
                snprintf(wifi_txt, wifi_sz, "%s %ddBm", ssid, ap.rssi);
            }
        } else {
            snprintf(wifi_txt, wifi_sz, "assoc %ddBm", ap.rssi);
        }
        esp_netif_t *na = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (na) {
            esp_netif_ip_info_t ipi;
            if (esp_netif_get_ip_info(na, &ipi) == ESP_OK && ipi.ip.addr != 0) {
                snprintf(ip_txt, ip_sz, IPSTR, IP2STR(&ipi.ip));
            } else {
                snprintf(ip_txt, ip_sz, "pending");
            }
        } else {
            snprintf(ip_txt, ip_sz, "no if");
        }
    } else {
        snprintf(wifi_txt, wifi_sz, "[wifi] idle");
        snprintf(ip_txt, ip_sz, "--.--.--.--");
    }
}

static void home_status_apply_labels(const char *wifi_txt, const char *ip_txt, const char *bat_txt, lv_color_t bat_col)
{
    lv_label_set_text(s_lbl_wifi, wifi_txt);
    lv_label_set_text(s_lbl_ip, ip_txt);
    if (s_lbl_bat) {
        if (s_home_bat_text_style_inited) {
            lv_style_set_text_color(&s_home_bat_text_style, bat_col);
        } else {
            lv_obj_set_style_text_color(s_lbl_bat, bat_col, 0);
        }
        lv_label_set_text(s_lbl_bat, bat_txt);
        lv_obj_invalidate(s_lbl_bat);
    }
}

static void home_status_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_lbl_wifi || !s_lbl_ip || !s_lbl_bat) {
        return;
    }
    if (!lv_obj_is_valid(s_lbl_wifi) || !lv_obj_is_valid(s_lbl_ip) || !lv_obj_is_valid(s_lbl_bat)) {
        dashcdg_home_ui_pause_status_updates();
        return;
    }

    char wifi_txt[48];
    char ip_txt[20];
    char bat_txt[28];
    lv_color_t bat_col;
    home_status_gather(wifi_txt, sizeof(wifi_txt), ip_txt, sizeof(ip_txt), bat_txt, sizeof(bat_txt), &bat_col);
    home_status_apply_labels(wifi_txt, ip_txt, bat_txt, bat_col);
}

static void home_status_refresh_foreign(void)
{
    if (!s_lbl_wifi || !s_lbl_ip || !s_lbl_bat) {
        return;
    }

    char wifi_txt[48];
    char ip_txt[20];
    char bat_txt[28];
    lv_color_t bat_col;
    home_status_gather(wifi_txt, sizeof(wifi_txt), ip_txt, sizeof(ip_txt), bat_txt, sizeof(bat_txt), &bat_col);

    if (!lvgl_port_lock(400)) {
        return;
    }
    home_status_apply_labels(wifi_txt, ip_txt, bat_txt, bat_col);
    lvgl_port_unlock();
}

/** Compact tile: in-button label only. */
static lv_obj_t *make_app_tile(lv_obj_t *parent, const char *btn_lines, lv_event_cb_t cb, lv_disp_t *disp)
{
    lv_obj_t *col = lv_obj_create(parent);
    /* Never flex_grow the wrapper in a column: two grow siblings split free height and leave a
     * huge empty band between stacked tiles. */
    lv_obj_set_width(col, lv_pct(100));
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(col, 3, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    dashcdg_ui_no_scroll(col);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *tile = lv_button_create(col);
    lv_obj_set_width(tile, lv_pct(100));
    lv_obj_set_height(tile, 54);
    lv_obj_set_style_radius(tile, 6, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x0a1410), 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(0x00aa55), 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_shadow_width(tile, 4, 0);
    lv_obj_set_style_shadow_color(tile, lv_color_hex(0x002211), 0);
    lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, disp);

    lv_obj_t *lg = lv_label_create(tile);
    lv_label_set_long_mode(lg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(lg, lv_pct(94));
    lv_label_set_text(lg, btn_lines);
    lv_obj_set_style_text_align(lg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lg, lv_color_hex(0x66ffaa), 0);
    lv_obj_center(lg);

    return col;
}

esp_err_t dashcdg_home_ui_present(lv_disp_t *disp)
{
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_ARG, TAG, "disp");

    dashcdg_home_ui_pause_status_updates();

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    dashcdg_display_clear_top_layer(disp);

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x020403), 0);
    dashcdg_ui_no_scroll(scr);
    /* Default screen is clickable and full-screen; if hit-testing misses children, LVGL targets scr and taps die. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *outer = lv_obj_create(scr);
    lv_obj_set_size(outer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(outer, 6, 0);
    lv_obj_set_style_border_width(outer, 0, 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    dashcdg_ui_no_scroll(outer);
    lv_obj_remove_flag(outer, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hdr = lv_label_create(outer);
    lv_label_set_text(hdr, "> Shadyvision // BADGE");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0x33ff99), 0);

    lv_obj_t *sub = lv_label_create(outer);
    lv_label_set_text(sub, "// pick exploit module ;)");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x558866), 0);

    /* Two full-width tiles only: karaoke, then settings (no flex-grow spacer — it ate layout and
     * pushed the status dock off-screen / clipped tiles). */
    lv_obj_t *tiles = lv_obj_create(outer);
    lv_obj_set_width(tiles, lv_pct(100));
    lv_obj_set_height(tiles, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(tiles, 2, 0);
    lv_obj_set_style_border_width(tiles, 0, 0);
    lv_obj_set_style_bg_opa(tiles, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tiles, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tiles, 6, 0);
    dashcdg_ui_no_scroll(tiles);
    lv_obj_remove_flag(tiles, LV_OBJ_FLAG_CLICKABLE);
    make_app_tile(tiles, "[ mcast ]\nrx/karaoke", on_tile_karaoke, disp);
    make_app_tile(tiles, "[ cfg ]\nsettings", on_tile_settings, disp);

    /* Push status bar to the physical bottom; only this spacer flex-grows (not inside tile column). */
    lv_obj_t *dock_spacer = lv_obj_create(outer);
    lv_obj_set_width(dock_spacer, lv_pct(100));
    lv_obj_set_flex_grow(dock_spacer, 1);
    lv_obj_set_style_min_height(dock_spacer, 0, 0);
    lv_obj_set_style_bg_opa(dock_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dock_spacer, 0, 0);
    dashcdg_ui_no_scroll(dock_spacer);
    lv_obj_remove_flag(dock_spacer, LV_OBJ_FLAG_CLICKABLE);

    /* Status dock: single-line fields + info btn */
    lv_obj_t *bar = lv_obj_create(outer);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 30);
    lv_obj_set_style_pad_left(bar, 6, 0);
    lv_obj_set_style_pad_right(bar, 4, 0);
    lv_obj_set_style_pad_top(bar, 4, 0);
    lv_obj_set_style_pad_bottom(bar, 4, 0);
    /* Inner width is hres minus outer pad minus bar pad; fixed children + gaps must fit (was
     * clipping the trailing "?" control on 320px landscape). */
    lv_obj_set_style_pad_column(bar, 3, 0);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x030605), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x115533), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_grow(bar, 0);
    dashcdg_ui_no_scroll(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    s_lbl_wifi = lv_label_create(bar);
    lv_label_set_long_mode(s_lbl_wifi, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(s_lbl_wifi, 90);
    lv_obj_set_height(s_lbl_wifi, 18);
    lv_obj_set_style_text_color(s_lbl_wifi, lv_color_hex(0x66ddaa), 0);
    lv_label_set_text(s_lbl_wifi, "[wifi] ...");
    lv_obj_add_flag(s_lbl_wifi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_lbl_wifi, on_tile_wifi, LV_EVENT_CLICKED, disp);

    s_lbl_ip = lv_label_create(bar);
    lv_label_set_long_mode(s_lbl_ip, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(s_lbl_ip, 72);
    lv_obj_set_height(s_lbl_ip, 18);
    lv_obj_set_style_text_color(s_lbl_ip, lv_color_hex(0xaaccff), 0);
    lv_obj_set_style_text_align(s_lbl_ip, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_lbl_ip, "--");

    s_lbl_bat = lv_label_create(bar);
    lv_label_set_long_mode(s_lbl_bat, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(s_lbl_bat, 94);
    lv_obj_set_height(s_lbl_bat, 18);
    home_bat_text_style_attach(s_lbl_bat);
    lv_obj_set_style_text_align(s_lbl_bat, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(s_lbl_bat, " -- ");

    lv_obj_t *sp = lv_obj_create(bar);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp, 0, 0);
    dashcdg_ui_no_scroll(sp);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *info = lv_button_create(bar);
    lv_obj_set_size(info, 26, 22);
    lv_obj_set_style_pad_all(info, 0, 0);
    lv_obj_set_style_shadow_width(info, 0, 0);
    lv_obj_set_style_bg_color(info, lv_color_hex(0x0a1510), 0);
    lv_obj_set_style_border_color(info, lv_color_hex(0x338866), 0);
    lv_obj_set_style_border_width(info, 1, 0);
    lv_obj_set_style_radius(info, 3, 0);
    lv_obj_t *il = lv_label_create(info);
    lv_label_set_text(il, "?");
    lv_obj_set_style_text_color(il, lv_color_hex(0x88ffcc), 0);
    lv_obj_center(il);
    lv_obj_add_event_cb(info, on_info_btn, LV_EVENT_CLICKED, disp);

    lv_obj_update_layout(outer);
    lv_obj_invalidate(scr);
    /* touch_cal_ui disables the indev; after lv_obj_clean, drop stale press/hover on deleted objs. */
    dashcdg_touch_rearm_locked();

#if DASHCDG_HOME_TOUCH_DEBUG_OVERLAY
    s_touch_dbg_lbl = lv_label_create(scr);
    lv_label_set_long_mode(s_touch_dbg_lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(s_touch_dbg_lbl, 316, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_touch_dbg_lbl, 2, 2);
    lv_obj_set_style_text_color(s_touch_dbg_lbl, lv_color_hex(0xffcc00), 0);
    lv_obj_set_style_bg_color(s_touch_dbg_lbl, lv_color_hex(0x0a0500), 0);
    lv_obj_set_style_bg_opa(s_touch_dbg_lbl, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(s_touch_dbg_lbl, 4, 0);
    lv_obj_set_style_pad_ver(s_touch_dbg_lbl, 2, 0);
    lv_obj_set_style_radius(s_touch_dbg_lbl, 2, 0);
    dashcdg_ui_no_scroll(s_touch_dbg_lbl);
    lv_obj_remove_flag(s_touch_dbg_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(s_touch_dbg_lbl, "tp dbg...");
    lv_obj_move_foreground(s_touch_dbg_lbl);
    if (s_touch_dbg_timer) {
        lv_timer_del(s_touch_dbg_timer);
        s_touch_dbg_timer = NULL;
    }
    s_touch_dbg_timer = lv_timer_create(home_touch_dbg_timer_cb, 80, NULL);
#endif

    lvgl_port_unlock();

    home_status_refresh_foreign();

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    s_status_timer = lv_timer_create(home_status_timer_cb, 2000, NULL);
    lvgl_port_unlock();

    return ESP_OK;
}
