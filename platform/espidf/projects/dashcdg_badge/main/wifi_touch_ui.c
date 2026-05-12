/*
 * Wi-Fi provisioning UI on LVGL + touch. Credentials stored in NVS namespace "dashcfg".
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "badge_ui_flair.h"
#include "display_lvgl.h"
#include "nav.h"
#include "platform_hw.h"
#include "badge_rx.h"
#include "wifi_touch_ui.h"

static const char *TAG = "wifi_ui";
static bool s_wifi_driver_ready;

/** IDF may re-apply default modem PS on (re)association; keep multicast RX from sleeping the radio. */
static void wifi_touch_clamp_ps_none_if_rx_active(void)
{
    if (dashcdg_badge_rx_is_running()) {
        (void)esp_wifi_set_ps(WIFI_PS_NONE);
    }
}

/** ESP-IDF `wifi_config_t` SSID/password are fixed arrays; avoid `strncpy(..., n-1)` — GCC stringop-truncation. */
static void wifi_touch_copy_to_cfg_field(uint8_t *dst, size_t dst_sz, const char *src)
{
    if (dst == NULL || dst_sz == 0U) {
        return;
    }
    snprintf((char *)dst, dst_sz, "%s", (src != NULL) ? src : "");
}
static esp_netif_t *s_wifi_sta_netif;
static const char *NVS_NS = "dashcfg";

/** Background STA reconnection when link drops; long random sleep 2–5 s between attempts. */
static TaskHandle_t s_reconn_task;
#define WIFI_RECONN_STACK_WORDS 4096
#define WIFI_RECONN_TASK_PRIO     3

static lv_obj_t *s_lbl_status;
static lv_obj_t *s_dd_ssid;
static lv_obj_t *s_ta_pass;
static lv_obj_t *s_kb;
static lv_obj_t *s_entry_row;

#if CONFIG_DASHCDG_BADGE_DEBUG_AUTO_LAUNCH_KARAOKE_AFTER_DHCP
static bool s_dbg_auto_karaoke_done_this_boot;
static bool s_dbg_auto_karaoke_dhcp_seen;
static bool s_dbg_auto_karaoke_launch_posted;

/**
 * Runs on the LVGL thread via `lv_async_call`. Resolves `lv_display_get_default()` here so DHCP before
 * display registration does not pass a stale/wrong pointer from `IP_EVENT` (sys_evt task).
 */
static void dbg_auto_karaoke_launch_async(void *unused)
{
    (void)unused;
    lv_disp_t *disp = lv_display_get_default();

    if (!disp) {
        /* DHCP beat LVGL registration — clear posted so `try_autolaunch_after_home` can retry. */
        s_dbg_auto_karaoke_launch_posted = false;
        return;
    }
    if (s_dbg_auto_karaoke_done_this_boot || !s_dbg_auto_karaoke_dhcp_seen) {
        s_dbg_auto_karaoke_launch_posted = false;
        return;
    }
    ESP_LOGI(TAG, "debug auto-karaoke: launching after DHCP");
    dashcdg_nav_karaoke(disp);
    s_dbg_auto_karaoke_done_this_boot = true;
    s_dbg_auto_karaoke_launch_posted = false;
}

static void dbg_auto_karaoke_schedule_launch_once(void)
{
    if (s_dbg_auto_karaoke_done_this_boot || s_dbg_auto_karaoke_launch_posted) {
        return;
    }
    s_dbg_auto_karaoke_launch_posted = true;
    lv_async_call(dbg_auto_karaoke_launch_async, NULL);
}
#endif /* CONFIG_DASHCDG_BADGE_DEBUG_AUTO_LAUNCH_KARAOKE_AFTER_DHCP */

static bool wifi_touch_ui_is_active(void)
{
    /* s_lbl_status is cleared in dashcdg_wifi_drop_lvgl_refs when leaving this screen. */
    return s_lbl_status != NULL;
}

void dashcdg_wifi_drop_lvgl_refs(void)
{
    s_lbl_status = NULL;
    s_dd_ssid = NULL;
    s_ta_pass = NULL;
    s_kb = NULL;
    s_entry_row = NULL;
}

static void ui_statusf(const char *fmt, ...)
{
    if (!s_lbl_status) {
        return;
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (lvgl_port_lock(1000)) {
        lv_label_set_text(s_lbl_status, buf);
        lvgl_port_unlock();
    }
}

static esp_err_t nvs_load_creds(char *ssid, size_t ssid_sz, char *psk, size_t psk_sz)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t l = ssid_sz;
    err = nvs_get_str(h, "ssid", ssid, &l);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    l = psk_sz;
    err = nvs_get_str(h, "psk", psk, &l);
    nvs_close(h);
    return err;
}

static bool nvs_has_saved_creds(void)
{
    char ssid[65] = {0};
    char psk[65] = {0};
    return nvs_load_creds(ssid, sizeof(ssid), psk, sizeof(psk)) == ESP_OK;
}

static esp_err_t nvs_save_creds(const char *ssid, const char *psk)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "ssid", ssid), TAG, "set ssid");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "psk", psk), TAG, "set psk");
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static esp_err_t nvs_clear_creds(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "psk");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Scan results + option string are large; event-loop task stack is ~2-3 KiB - keep off stack. */
static wifi_ap_record_t s_scan_recs[40];
static char s_scan_opts[2048];

static void rebuild_dropdown_from_scan(void)
{
    memset(s_scan_recs, 0, sizeof(s_scan_recs));
    uint16_t n = 40;
    esp_err_t err = esp_wifi_scan_get_ap_records(&n, s_scan_recs);
    if (err != ESP_OK || n == 0) {
        ui_statusf("Scan: no APs (%s)", esp_err_to_name(err));
        return;
    }

    size_t off = 0;
    off += snprintf(s_scan_opts + off, sizeof(s_scan_opts) - off, "%s", "(select SSID)");

    for (unsigned i = 0; i < n && off < sizeof(s_scan_opts) - 64; i++) {
        char line[40];
        const uint8_t *s = s_scan_recs[i].ssid;
        size_t sl = strnlen((const char *)s, sizeof(s_scan_recs[i].ssid));
        if (sl == 0) {
            continue;
        }
        memcpy(line, s, sl);
        line[sl] = 0;
        off += snprintf(s_scan_opts + off, sizeof(s_scan_opts) - off, "\n%s", line);
    }

    if (lvgl_port_lock(1000)) {
        lv_dropdown_set_options(s_dd_ssid, s_scan_opts);
        lvgl_port_unlock();
    }
    ui_statusf("Scan: found networks - pick SSID");
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        rebuild_dropdown_from_scan();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ui_statusf("Wi-Fi started");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (nvs_has_saved_creds()) {
            ui_statusf("Disconnected\n(retry every 2–5 s in background)");
        } else {
            ui_statusf("Disconnected\n(tap Connect after Scan)");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        /* Fresh DHCP on each association avoids stale lwIP client state / odd subnets on some APs. */
        esp_netif_t *na = s_wifi_sta_netif ? s_wifi_sta_netif : esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (na) {
            (void)esp_netif_dhcpc_stop(na);
            esp_err_t d = esp_netif_dhcpc_start(na);
            if (d != ESP_OK) {
                ESP_LOGW(TAG, "dhcpc_start after STA_CONNECTED: %s", esp_err_to_name(d));
            }
        }
        wifi_touch_clamp_ps_none_if_rx_active();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ui_statusf("Online\nIP: " IPSTR, IP2STR(&ev->ip_info.ip));
        /* OpenWrt mcast-to-unicast: re-bind STA UDP + IGMP after DHCP (RX may have started with no IP). */
        wifi_touch_clamp_ps_none_if_rx_active();
        dashcdg_badge_rx_notify_sta_got_ip();

        wifi_config_t wc = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) {
            nvs_save_creds((const char *)wc.sta.ssid, (const char *)wc.sta.password);
        }
        dashcdg_wifi_debug_on_sta_got_ip();
    }
}

static void on_scan(lv_event_t *e)
{
    (void)e;
    ui_statusf("Scanning...");
    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        ui_statusf("Scan start failed: %s", esp_err_to_name(err));
    }
}

static void on_connect(lv_event_t *e)
{
    (void)e;
    char ssid[33] = {0};
    char psk[65] = {0};

    if (lvgl_port_lock(1000)) {
        lv_dropdown_get_selected_str(s_dd_ssid, ssid, sizeof(ssid));
        const char *pw = lv_textarea_get_text(s_ta_pass);
        snprintf(psk, sizeof(psk), "%s", pw != NULL ? pw : "");
        lvgl_port_unlock();
    }

    if (ssid[0] == 0 || strcmp(ssid, "(select SSID)") == 0) {
        ui_statusf("Pick an SSID from the list");
        return;
    }

    wifi_config_t wc = {0};
    wifi_touch_copy_to_cfg_field(wc.sta.ssid, sizeof(wc.sta.ssid), ssid);
    wifi_touch_copy_to_cfg_field(wc.sta.password, sizeof(wc.sta.password), psk);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.listen_interval = 1;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ui_statusf("set_config: %s", esp_err_to_name(err));
        return;
    }
    ui_statusf("Connecting to\n%s ...", ssid);
    err = esp_wifi_disconnect();
    (void)err;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ui_statusf("connect: %s", esp_err_to_name(err));
    }
}

static void on_forget(lv_event_t *e)
{
    (void)e;
    (void)nvs_clear_creds();
    esp_wifi_disconnect();
    ui_statusf("Forgot saved Wi-Fi");
    if (lvgl_port_lock(1000)) {
        lv_textarea_set_text(s_ta_pass, "");
        lvgl_port_unlock();
    }
}

static void hide_passphrase_ui(void)
{
    if (!s_kb || !s_entry_row || !s_ta_pass) {
        return;
    }
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_entry_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(s_ta_pass, LV_STATE_FOCUSED);
}

static void on_ta_ready(lv_event_t *e)
{
    (void)e;
    if (lvgl_port_lock(1000)) {
        hide_passphrase_ui();
        lvgl_port_unlock();
    }
}

static void on_ta_cancel(lv_event_t *e)
{
    (void)e;
    if (lvgl_port_lock(1000)) {
        hide_passphrase_ui();
        lvgl_port_unlock();
    }
}

static void on_pass(lv_event_t *e)
{
    (void)e;
    if (!lvgl_port_lock(1000)) {
        return;
    }
    lv_obj_remove_flag(s_entry_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_kb, s_ta_pass);
    lv_obj_add_state(s_ta_pass, LV_STATE_FOCUSED);
    lvgl_port_unlock();
}

static void on_nav_home(lv_event_t *e)
{
    lv_disp_t *disp = lv_event_get_user_data(e);
    if (disp) {
        dashcdg_nav_home(disp);
    }
}

static void build_ui(lv_disp_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    /*
     * Landscape 320x240: scrollable setup + optional passphrase row + keyboard docked at bottom.
     * Keyboard stays outside the scroll area (reliable touch). Passphrase line + keyboard are
     * hidden until Pass is tapped; OK on the keyboard sends READY and hides them.
     */
    lv_obj_t *outer = lv_obj_create(scr);
    lv_obj_set_size(outer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(outer, 4, 0);
    lv_obj_set_style_border_width(outer, 0, 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(outer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(outer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *root = lv_obj_create(outer);
    lv_obj_set_width(root, lv_pct(100));
    lv_obj_set_flex_grow(root, 1);
    lv_obj_set_style_min_height(root, 48, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_bottom(root, 6, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(root, 4, 0);
    lv_obj_set_scroll_dir(root, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *nav_row = lv_obj_create(root);
    lv_obj_set_width(nav_row, lv_pct(100));
    lv_obj_set_height(nav_row, 38);
    lv_obj_set_style_pad_all(nav_row, 0, 0);
    lv_obj_set_style_border_width(nav_row, 0, 0);
    lv_obj_set_style_bg_opa(nav_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *b_home = lv_button_create(nav_row);
    lv_obj_set_width(b_home, 72);
    lv_obj_t *lh = lv_label_create(b_home);
    lv_label_set_text(lh, "Home");
    lv_obj_center(lh);
    lv_obj_add_event_cb(b_home, on_nav_home, LV_EVENT_CLICKED, disp);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Wi-Fi Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0xb0ffe8), 0);

    lv_obj_t *wsub = lv_label_create(root);
    lv_label_set_text(wsub, dashcdg_ui_flair_wifi_sub());
    lv_label_set_long_mode(wsub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(wsub, lv_pct(100));
    lv_obj_set_style_text_color(wsub, lv_color_hex(0x669988), 0);

    s_lbl_status = lv_label_create(root);
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_status, lv_pct(100));
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xe0e0e8), 0);
    lv_label_set_text(s_lbl_status, "Ready");

    s_dd_ssid = lv_dropdown_create(root);
    lv_obj_set_width(s_dd_ssid, lv_pct(100));
    lv_dropdown_set_options(s_dd_ssid, "(select SSID)\nTap SCAN");

    lv_obj_t *row = lv_obj_create(root);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 42);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, 0);

    lv_obj_t *b_scan = lv_button_create(row);
    lv_obj_set_flex_grow(b_scan, 1);
    lv_obj_t *l1 = lv_label_create(b_scan);
    lv_label_set_text(l1, "Scan");
    lv_obj_center(l1);
    lv_obj_add_event_cb(b_scan, on_scan, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_go = lv_button_create(row);
    lv_obj_set_flex_grow(b_go, 1);
    lv_obj_t *l2 = lv_label_create(b_go);
    lv_label_set_text(l2, "Connect");
    lv_obj_center(l2);
    lv_obj_add_event_cb(b_go, on_connect, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_forget = lv_button_create(row);
    lv_obj_set_flex_grow(b_forget, 1);
    lv_obj_t *l3 = lv_label_create(b_forget);
    lv_label_set_text(l3, "Forget");
    lv_obj_center(l3);
    lv_obj_add_event_cb(b_forget, on_forget, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_pass = lv_button_create(row);
    lv_obj_set_flex_grow(b_pass, 1);
    lv_obj_t *l4 = lv_label_create(b_pass);
    lv_label_set_text(l4, "Pass");
    lv_obj_center(l4);
    lv_obj_add_event_cb(b_pass, on_pass, LV_EVENT_CLICKED, NULL);

    s_entry_row = lv_obj_create(outer);
    lv_obj_set_width(s_entry_row, lv_pct(100));
    lv_obj_set_height(s_entry_row, 36);
    lv_obj_set_style_pad_all(s_entry_row, 2, 0);
    lv_obj_set_style_border_width(s_entry_row, 0, 0);
    lv_obj_set_style_bg_opa(s_entry_row, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_entry_row, LV_OBJ_FLAG_HIDDEN);

    s_ta_pass = lv_textarea_create(s_entry_row);
    lv_obj_set_width(s_ta_pass, lv_pct(100));
    lv_obj_set_height(s_ta_pass, LV_SIZE_CONTENT);
    lv_textarea_set_one_line(s_ta_pass, true);
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_textarea_set_placeholder_text(s_ta_pass, "WPA passphrase");
    lv_textarea_set_cursor_click_pos(s_ta_pass, true);
    lv_obj_add_event_cb(s_ta_pass, on_ta_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_ta_pass, on_ta_cancel, LV_EVENT_CANCEL, NULL);

    s_kb = lv_keyboard_create(outer);
    lv_obj_set_width(s_kb, lv_pct(100));
    /* Landscape: use vertical space for wider, taller keys; cap keeps layout stable. */
    lv_obj_set_style_max_height(s_kb, 158, 0);
    lv_obj_align(s_kb, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_keyboard_set_textarea(s_kb, s_ta_pass);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);

    lv_obj_update_layout(outer);
}

static esp_err_t try_auto_connect_saved(void)
{
    char ssid[65] = {0};
    char psk[65] = {0};
    if (nvs_load_creds(ssid, sizeof(ssid), psk, sizeof(psk)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t wc = {0};
    wifi_touch_copy_to_cfg_field(wc.sta.ssid, sizeof(wc.sta.ssid), ssid);
    wifi_touch_copy_to_cfg_field(wc.sta.password, sizeof(wc.sta.password), psk);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    /* Min listen interval (beacon periods): if modem PS is ever active, wake often for DTIM/mcast. */
    wc.sta.listen_interval = 1;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set saved");
    ui_statusf("Auto-connect...\n%s", ssid);
    return esp_wifi_connect();
}

/**
 * Same as try_auto_connect_saved without LVGL status (for background task).
 * Uses saved NVS SSID/PSK and current STA config path as the touch UI.
 */
static esp_err_t wifi_reconnect_apply_saved(void)
{
    char ssid[65] = {0};
    char psk[65] = {0};
    if (nvs_load_creds(ssid, sizeof(ssid), psk, sizeof(psk)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    wifi_config_t wc = {0};
    wifi_touch_copy_to_cfg_field(wc.sta.ssid, sizeof(wc.sta.ssid), ssid);
    wifi_touch_copy_to_cfg_field(wc.sta.password, sizeof(wc.sta.password), psk);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.listen_interval = 1;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "auto-reconnect set_config: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        /* ESP_ERR_WIFI_CONN: already connecting — ignore noise. */
        ESP_LOGD(TAG, "auto-reconnect esp_wifi_connect -> %s", esp_err_to_name(err));
    }
    return err;
}

static void wifi_reconn_task_fn(void *arg)
{
    (void)arg;

    for (;;) {
        /* Time-bounded backoff: uniform random in [2000, 5000] ms (no tight spin). */
        uint32_t wait_ms = 2000U + (esp_random() % 3001U);
        vTaskDelay(pdMS_TO_TICKS(wait_ms));

        if (!s_wifi_driver_ready) {
            continue;
        }
        /* Avoid fighting the Wi-Fi setup screen (scan / manual connect). */
        if (wifi_touch_ui_is_active()) {
            continue;
        }
        if (!nvs_has_saved_creds()) {
            continue;
        }
        {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                continue;
            }
        }
        (void)wifi_reconnect_apply_saved();
    }
}

static void wifi_reconn_task_start_once(void)
{
    if (s_reconn_task != NULL) {
        return;
    }
    if (xTaskCreate(wifi_reconn_task_fn, "wifi_reconn", WIFI_RECONN_STACK_WORDS, NULL, WIFI_RECONN_TASK_PRIO,
                    &s_reconn_task) != pdPASS) {
        ESP_LOGW(TAG, "wifi_reconn task create failed");
        s_reconn_task = NULL;
    } else {
        ESP_LOGI(TAG, "wifi_reconn: background reconnect every 2–5 s when disconnected + creds saved");
    }
}

esp_err_t dashcdg_wifi_boot_auto_connect(void)
{
    ESP_RETURN_ON_ERROR(dashcdg_wifi_ensure_init(), TAG, "wifi init");
    return try_auto_connect_saved();
}

esp_err_t dashcdg_wifi_ensure_init(void)
{
    if (s_wifi_driver_ready) {
        return ESP_OK;
    }

#if CONFIG_DASHCDG_BADGE_DEBUG_AUTO_LAUNCH_KARAOKE_AFTER_DHCP
    s_dbg_auto_karaoke_done_this_boot = false;
    s_dbg_auto_karaoke_dhcp_seen = false;
    s_dbg_auto_karaoke_launch_posted = false;
#endif

    s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_sta_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }
    esp_netif_set_default_netif(s_wifi_sta_netif);

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL), TAG,
                        "reg wifi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL), TAG,
                        "reg ip");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode sta");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");

    s_wifi_driver_ready = true;
    wifi_reconn_task_start_once();
    return ESP_OK;
}

esp_err_t dashcdg_wifi_touch_ui_present(lv_disp_t *disp)
{
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_ERR_INVALID_ARG, TAG, "disp");
    ESP_RETURN_ON_ERROR(dashcdg_wifi_ensure_init(), TAG, "wifi init");

    dashcdg_wifi_drop_lvgl_refs();

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    dashcdg_display_clear_top_layer(disp);

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_clean(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_CLICKABLE);

    build_ui(disp);
    lv_obj_invalidate(scr);
    lvgl_port_unlock();

    try_auto_connect_saved();

    dashcdg_platform_hw_set_screen(DASHCDG_HW_SCREEN_WIFI);
    return ESP_OK;
}

esp_err_t dashcdg_wifi_touch_ui_start(lv_disp_t *disp)
{
    return dashcdg_wifi_touch_ui_present(disp);
}

void dashcdg_wifi_debug_on_sta_got_ip(void)
{
#if CONFIG_DASHCDG_BADGE_DEBUG_AUTO_LAUNCH_KARAOKE_AFTER_DHCP
    if (s_dbg_auto_karaoke_done_this_boot) {
        return;
    }
    s_dbg_auto_karaoke_dhcp_seen = true;
    dbg_auto_karaoke_schedule_launch_once();
#endif
}

void dashcdg_wifi_debug_try_autolaunch_after_home(lv_disp_t *disp)
{
#if CONFIG_DASHCDG_BADGE_DEBUG_AUTO_LAUNCH_KARAOKE_AFTER_DHCP
    esp_netif_ip_info_t ipi;

    if (!disp || s_dbg_auto_karaoke_done_this_boot) {
        return;
    }
    {
        esp_netif_t *na = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

        if (na == NULL || esp_netif_get_ip_info(na, &ipi) != ESP_OK || ipi.ip.addr == 0U) {
            return;
        }
    }
    /* STA already had a lease before LVGL finished — arm the same one-shot path as GOT_IP. */
    s_dbg_auto_karaoke_dhcp_seen = true;
    dbg_auto_karaoke_schedule_launch_once();
#else
    (void)disp;
#endif
}
