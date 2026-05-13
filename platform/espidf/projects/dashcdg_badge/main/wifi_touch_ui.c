/*
 * Wi-Fi provisioning UI on LVGL + touch. Credentials stored in NVS namespace "dashcfg".
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <inttypes.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "badge_exec.h"
#include "badge_ui_flair.h"
#include "display_lvgl.h"
#include "nav.h"
#include "platform_hw.h"
#include "badge_rx.h"
#include "esp_timer.h"
#include "wifi_touch_ui.h"

/*
 * DHCP timeout for the boot orchestrator: armed when the saved-credentials connect succeeds, fired
 * once if IP_EVENT_STA_GOT_IP hasn't arrived in CONFIG_DASHCDG_BADGE_EXEC_WIFI_DHCP_TIMEOUT_MS, and
 * cancelled the moment we see GOT_IP. We only arm once per boot: the BOOT_WIFI_DHCP_TIMEOUT bit is
 * a latched boot fact, not a runtime "is the link healthy?" signal.
 */
#ifndef CONFIG_DASHCDG_BADGE_EXEC_WIFI_DHCP_TIMEOUT_MS
#define CONFIG_DASHCDG_BADGE_EXEC_WIFI_DHCP_TIMEOUT_MS 10000
#endif
static esp_timer_handle_t s_boot_dhcp_timer;
static bool s_boot_dhcp_armed;
static bool s_boot_wifi_got_ip;

static void boot_dhcp_timer_cb(void *arg)
{
    (void)arg;
    if (s_boot_wifi_got_ip) {
        return;
    }
    (void)dashcdg_badge_exec_publish_boot_event(DASHCDG_BADGE_EXEC_BOOT_WIFI_DHCP_TIMEOUT,
                                                "no_got_ip_within_timeout");
    (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_WIFI_STA,
                                        DASHCDG_BADGE_EXEC_HEALTH_TIMEOUT, "dhcp_timeout");
}

static void boot_dhcp_timer_arm_once(void)
{
    if (s_boot_dhcp_armed || s_boot_wifi_got_ip) {
        return;
    }
    if (s_boot_dhcp_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = boot_dhcp_timer_cb,
            .name = "boot_dhcp_to",
        };
        if (esp_timer_create(&args, &s_boot_dhcp_timer) != ESP_OK) {
            s_boot_dhcp_timer = NULL;
            return;
        }
    }
    if (esp_timer_start_once(s_boot_dhcp_timer,
                             (uint64_t)CONFIG_DASHCDG_BADGE_EXEC_WIFI_DHCP_TIMEOUT_MS * 1000ULL) == ESP_OK) {
        s_boot_dhcp_armed = true;
    }
}

static void boot_dhcp_timer_cancel(void)
{
    if (s_boot_dhcp_timer != NULL && s_boot_dhcp_armed) {
        (void)esp_timer_stop(s_boot_dhcp_timer);
        s_boot_dhcp_armed = false;
    }
}

/*
 * TAG is used by both the legacy wifi/UI code further down AND by the wifi_owner queue
 * helpers immediately below; declare once here so both compile cleanly. (T3 left a forward
 * reference behind that landed in commit but only got caught on a fresh build with no ccache.)
 */
static const char *TAG = "wifi_ui";

/* Forward declarations for helpers defined further below that wifi_owner needs to call. */
static void rebuild_dropdown_from_scan(void);
static void ui_statusf(const char *fmt, ...);
static esp_err_t nvs_save_creds(const char *ssid, const char *psk);

/*
 * --------------------------------------------------------------------------
 *  wifi_owner: command queue + worker task for everything the ESP event task
 *  used to do directly.
 * --------------------------------------------------------------------------
 *
 * The ESP-IDF system event task runs all Wi-Fi/IP handlers we register with
 * the default event loop. It has a small stack (typically 2 KiB) and is
 * shared by every component, so it must not perform LVGL UI rebuilds, NVS
 * writes, or other heavy work. Before T3 our handler was doing exactly that.
 *
 * After T3 the event handler only:
 *   - publishes BOOT_* facts and SUB_WIFI_* health (cheap atomic / mutex
 *     copy work via badge_exec);
 *   - issues a couple of bounded IDF calls (esp_netif_dhcpc_*,
 *     wifi_touch_clamp_ps_none_if_rx_active);
 *   - posts a small command struct to wifi_owner's queue with
 *     xQueueSend(... 0 ticks) so a backed-up queue cannot stall the system
 *     event task.
 *
 * wifi_owner drains the queue, performs LVGL status updates,
 * rebuilds the SSID dropdown, saves credentials to NVS on first GOT_IP,
 * notifies badge_rx, and runs the optional Kconfig debug auto-launch.
 *
 * Queue depth is intentionally generous compared to expected event cadence
 * (scan complete + connect/disconnect bursts), and overflows are counted via
 * s_wifi_owner_q_drops + a single warning log per ticker boundary.
 */

#define WIFI_OWNER_TASK_STACK         4096U
#define WIFI_OWNER_TASK_PRIO          3U
#define WIFI_OWNER_QUEUE_DEPTH        16U
#define WIFI_OWNER_QUEUE_RX_WAIT_MS   2000U
#define WIFI_OWNER_QUEUE_TX_WAIT_TICKS 0  /* event handler: never block */
#if !CONFIG_FREERTOS_UNICORE
/* Keep Wi-Fi/UI background work on PRO core; RX/audio are pinned off it. */
#define WIFI_OWNER_TASK_CORE          0
#endif

typedef enum {
    WIFI_OWNER_CMD_SCAN_DONE = 1,
    WIFI_OWNER_CMD_STA_START,
    WIFI_OWNER_CMD_STA_DISCONNECTED,
    WIFI_OWNER_CMD_STA_GOT_IP,
} wifi_owner_cmd_kind_t;

typedef struct {
    wifi_owner_cmd_kind_t kind;
    /*
     * Raw esp_ip4_addr_t::addr (lwIP network-byte-order layout), valid for STA_GOT_IP. The owner
     * task feeds this straight into `struct in_addr` so inet_ntoa renders correctly on the badge.
     */
    uint32_t ipv4_be;
    bool has_saved_creds;     /* valid for STA_DISCONNECTED */
} wifi_owner_cmd_t;

static QueueHandle_t s_wifi_owner_q;
static TaskHandle_t s_wifi_owner_task;
static volatile uint32_t s_wifi_owner_q_drops;
static volatile uint32_t s_wifi_owner_q_high_water;

/*
 * Cached "do we have saved Wi-Fi credentials?" lookup so the ESP event task does not have to
 * pop into NVS on every WIFI_EVENT_STA_DISCONNECTED. Updated from nvs_save_creds /
 * try_auto_connect_saved / wifi_reconnect_apply_saved (all of which already read or wrote NVS) and
 * read with a volatile load from the event handler.
 */
static volatile bool s_has_saved_creds_cached;

static void wifi_owner_q_observe_high_water_locked(void)
{
    if (s_wifi_owner_q == NULL) {
        return;
    }
    UBaseType_t waiting = uxQueueMessagesWaiting(s_wifi_owner_q);
    if ((uint32_t)waiting > s_wifi_owner_q_high_water) {
        s_wifi_owner_q_high_water = (uint32_t)waiting;
    }
}

static void wifi_owner_post(const wifi_owner_cmd_t *cmd)
{
    if (s_wifi_owner_q == NULL || cmd == NULL) {
        return;
    }
    if (xQueueSend(s_wifi_owner_q, cmd, (TickType_t)WIFI_OWNER_QUEUE_TX_WAIT_TICKS) != pdTRUE) {
        s_wifi_owner_q_drops++;
        ESP_LOGW(TAG, "wifi_owner queue drop kind=%u drops=%" PRIu32, (unsigned)cmd->kind,
                 (uint32_t)s_wifi_owner_q_drops);
    }
    wifi_owner_q_observe_high_water_locked();
}

static void wifi_owner_handle_scan_done(void)
{
    rebuild_dropdown_from_scan();
}

static void wifi_owner_handle_sta_start(void)
{
    ui_statusf("Wi-Fi started");
}

static void wifi_owner_handle_sta_disconnected(bool has_saved_creds)
{
    if (has_saved_creds) {
        ui_statusf("Disconnected\n(retry every 2-5 s in background)");
    } else {
        ui_statusf("Disconnected\n(tap Connect after Scan)");
    }
}

static void wifi_owner_handle_sta_got_ip(uint32_t ipv4_be)
{
    struct in_addr ia = { .s_addr = ipv4_be };
    ui_statusf("Online\nIP: %s", inet_ntoa(ia));

    wifi_config_t wc = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK) {
        (void)nvs_save_creds((const char *)wc.sta.ssid, (const char *)wc.sta.password);
    }

    /*
     * notify RX *after* NVS save so a slow flash write does not delay the
     * IGMP-resync-on-DHCP path: NVS save runs here on wifi_owner, not in
     * the event task, and badge_rx_notify is a cheap flag flip.
     */
    dashcdg_badge_rx_notify_sta_got_ip();
    dashcdg_wifi_debug_on_sta_got_ip();
    dashcdg_badge_exec_task_progress("wifi_owner");
}

static void wifi_owner_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        wifi_owner_cmd_t cmd;
        BaseType_t got = xQueueReceive(s_wifi_owner_q, &cmd, pdMS_TO_TICKS(WIFI_OWNER_QUEUE_RX_WAIT_MS));
        dashcdg_badge_exec_task_heartbeat("wifi_owner");
        if (got != pdTRUE) {
            continue;
        }
        switch (cmd.kind) {
        case WIFI_OWNER_CMD_SCAN_DONE:
            wifi_owner_handle_scan_done();
            break;
        case WIFI_OWNER_CMD_STA_START:
            wifi_owner_handle_sta_start();
            break;
        case WIFI_OWNER_CMD_STA_DISCONNECTED:
            wifi_owner_handle_sta_disconnected(cmd.has_saved_creds);
            break;
        case WIFI_OWNER_CMD_STA_GOT_IP:
            wifi_owner_handle_sta_got_ip(cmd.ipv4_be);
            break;
        default:
            break;
        }
    }
}

static void wifi_owner_start_once(void)
{
    if (s_wifi_owner_task != NULL) {
        return;
    }
    if (s_wifi_owner_q == NULL) {
        s_wifi_owner_q = xQueueCreate(WIFI_OWNER_QUEUE_DEPTH, sizeof(wifi_owner_cmd_t));
        if (s_wifi_owner_q == NULL) {
            ESP_LOGE(TAG, "wifi_owner queue alloc failed");
            return;
        }
    }
    BaseType_t ok = xTaskCreate(wifi_owner_task_fn, "wifi_owner", WIFI_OWNER_TASK_STACK, NULL,
                                WIFI_OWNER_TASK_PRIO, &s_wifi_owner_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "wifi_owner task create failed");
        s_wifi_owner_task = NULL;
        return;
    }
    (void)dashcdg_badge_exec_register_task("wifi_owner", s_wifi_owner_task,
                                           (uint8_t)WIFI_OWNER_TASK_PRIO,
                                           (int8_t)-1,
                                           (uint16_t)WIFI_OWNER_TASK_STACK);
    ESP_LOGI(TAG, "wifi_owner up depth=%u prio=%u", (unsigned)WIFI_OWNER_QUEUE_DEPTH,
             (unsigned)WIFI_OWNER_TASK_PRIO);
}

static bool s_wifi_driver_ready;

/*
 * IDF may re-apply default modem PS on (re)association; keep multicast RX from sleeping the radio.
 * Previously we gated this on `dashcdg_badge_rx_is_running()` so the clamp only fired *after* RX
 * task came up — but STA_CONNECTED / STA_GOT_IP can land before that, leaving a brief PS_MIN_MODEM
 * window during which we drop multicast bursts (= audio chop and "JB stays at 100% / wm explodes"
 * symptom).  The initial clamp in `wifi_driver_init_only` covers boot; this one re-asserts after
 * every association event so a reconnect can never sneak PS back in.
 */
static void wifi_touch_clamp_ps_none_if_rx_active(void)
{
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
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
#if !CONFIG_FREERTOS_UNICORE
#define WIFI_RECONN_TASK_CORE     0
#endif

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

/*
 * Returns true if NVS currently has saved STA credentials. Touches flash via nvs_load_creds. We
 * keep the result in s_has_saved_creds_cached so the ESP event handler never has to call this
 * directly - it consults the cache instead.
 */
static bool nvs_has_saved_creds(void)
{
    char ssid[65] = {0};
    char psk[65] = {0};
    bool ok = (nvs_load_creds(ssid, sizeof(ssid), psk, sizeof(psk)) == ESP_OK);
    s_has_saved_creds_cached = ok;
    return ok;
}

static esp_err_t nvs_save_creds(const char *ssid, const char *psk)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "ssid", ssid), TAG, "set ssid");
    ESP_RETURN_ON_ERROR(nvs_set_str(h, "psk", psk), TAG, "set psk");
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) {
        s_has_saved_creds_cached = true;
    }
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
    if (err == ESP_OK) {
        s_has_saved_creds_cached = false;
    }
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

/*
 * ESP-IDF system event task callback. Per docs/specs/esp32-badge-freertos-executive-refactor-spec.md
 * section 5.1, this handler does only:
 *   - cheap badge_exec publish_boot_event / set_health calls,
 *   - a couple of fast IDF radio calls (dhcpc_stop/start, set_ps),
 *   - posts a wifi_owner_cmd_t with zero-tick send so a backed-up queue cannot stall the system
 *     event task.
 * LVGL UI rebuild, NVS save, and badge_rx mutation all happen on the wifi_owner task.
 */
static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        wifi_owner_cmd_t cmd = { .kind = WIFI_OWNER_CMD_SCAN_DONE };
        wifi_owner_post(&cmd);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        wifi_owner_cmd_t cmd = { .kind = WIFI_OWNER_CMD_STA_START };
        wifi_owner_post(&cmd);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_owner_cmd_t cmd = {
            .kind = WIFI_OWNER_CMD_STA_DISCONNECTED,
            .has_saved_creds = s_has_saved_creds_cached,
        };
        wifi_owner_post(&cmd);
        (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_WIFI_STA,
                                            DASHCDG_BADGE_EXEC_HEALTH_DEGRADED,
                                            "disconnected");
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
        (void)dashcdg_badge_exec_publish_boot_event(DASHCDG_BADGE_EXEC_BOOT_WIFI_CONNECTING,
                                                    "sta_connected_awaiting_ip");
        (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_WIFI_STA,
                                            DASHCDG_BADGE_EXEC_HEALTH_DEGRADED,
                                            "awaiting_ip");
        boot_dhcp_timer_arm_once();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        wifi_touch_clamp_ps_none_if_rx_active();
        s_boot_wifi_got_ip = true;
        boot_dhcp_timer_cancel();
        (void)dashcdg_badge_exec_publish_boot_event(DASHCDG_BADGE_EXEC_BOOT_WIFI_GOT_IP, "got_ip");
        (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_WIFI_STA,
                                            DASHCDG_BADGE_EXEC_HEALTH_OK, "got_ip");
        wifi_owner_cmd_t cmd = {
            .kind = WIFI_OWNER_CMD_STA_GOT_IP,
            .ipv4_be = ev->ip_info.ip.addr,
        };
        wifi_owner_post(&cmd);
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
        s_has_saved_creds_cached = false;
        (void)dashcdg_badge_exec_publish_boot_event(DASHCDG_BADGE_EXEC_BOOT_WIFI_NO_CREDS,
                                                    "no_saved_creds");
        (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_WIFI_STA,
                                            DASHCDG_BADGE_EXEC_HEALTH_DEGRADED, "no_saved_creds");
        return ESP_ERR_NOT_FOUND;
    }
    s_has_saved_creds_cached = true;

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

        /*
         * Loop-boundary heartbeat: this task spends almost all of its time blocked in
         * vTaskDelay() because we only attempt to reconnect when the link is actually down. The
         * heartbeat tells the liveness sweep that the task is alive even when no progress is
         * being made (steady-state when STA is connected). Real "no progress" stalls show up as a
         * lack of _task_progress() calls below, which only fire when wifi_reconnect_apply_saved
         * actually does work.
         */
        dashcdg_badge_exec_task_heartbeat("wifi_reconn");

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
        dashcdg_badge_exec_task_progress("wifi_reconn");
    }
}

static void wifi_reconn_task_start_once(void)
{
    if (s_reconn_task != NULL) {
        return;
    }
    BaseType_t ok = xTaskCreate(wifi_reconn_task_fn, "wifi_reconn", WIFI_RECONN_STACK_WORDS, NULL,
                                WIFI_RECONN_TASK_PRIO, &s_reconn_task);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "wifi_reconn task create failed");
        s_reconn_task = NULL;
    } else {
        ESP_LOGI(TAG, "wifi_reconn: background reconnect every 2–5 s when disconnected + creds saved");
        (void)dashcdg_badge_exec_register_task("wifi_reconn", s_reconn_task,
                                               (uint8_t)WIFI_RECONN_TASK_PRIO,
                                               (int8_t)-1,
                                               (uint16_t)WIFI_RECONN_STACK_WORDS);
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
    /*
     * ESP-IDF defaults `esp_wifi_start` to `WIFI_PS_MIN_MODEM` (the boot log shows
     * `wifi:pm start, type: 1` immediately after start).  For our multicast karaoke RX that
     * one-second-ish PS window starves the audio stream — we measured ~70 % audio chunk loss
     * with WIFI_PS_MIN active.  Clamp to NONE here so the radio is awake before STA connect /
     * IP_EVENT_STA_GOT_IP fires; downstream paths (badge_rx, karaoke entry) then trust it.
     */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    s_wifi_driver_ready = true;
    (void)dashcdg_badge_exec_publish_boot_event(DASHCDG_BADGE_EXEC_BOOT_WIFI_DRV_OK, "wifi_start_ok");
    (void)dashcdg_badge_exec_set_health(DASHCDG_BADGE_EXEC_SUB_WIFI_DRV,
                                        DASHCDG_BADGE_EXEC_HEALTH_OK, NULL);
    wifi_owner_start_once();
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
