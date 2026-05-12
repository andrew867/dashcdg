/*
 * badge_exec implementation - see badge_exec.h.
 *
 * Internal data layout (intentionally simple to keep all hot paths sub-microsecond):
 *
 *     +-------------------+
 *     | s_boot_evt_group  |   FreeRTOS EventGroup; latched BOOT_* bits.
 *     +-------------------+
 *
 *     +-------------------+
 *     | s_lock (mutex)    |   Guards s_health[] and s_tasks[]. Held only for trivial copies.
 *     +-------------------+
 *
 *     +-------------------+
 *     | s_health[N_SUB]   |   Per-subsystem state + last reason string + transition counter.
 *     +-------------------+
 *
 *     +-------------------+
 *     | s_tasks[SLOTS]    |   Task registry; in_use slots only. Heartbeats are written without
 *     |                   |   the mutex held by the task that owns the slot (single writer per
 *     |                   |   slot) so they cannot stall RX or LVGL.
 *     +-------------------+
 *
 * No API in this file blocks while holding the mutex except for the brief copy operations needed
 * to publish/read aggregate snapshots. Trace logging is performed outside the mutex window so a
 * slow UART (or future jsonl sink) does not back up health writes.
 */

#include "badge_exec.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "badge_exec";

#ifndef CONFIG_DASHCDG_BADGE_EXEC_TRACE
#define CONFIG_DASHCDG_BADGE_EXEC_TRACE 1
#endif

#ifndef CONFIG_DASHCDG_BADGE_EXEC_LOCK_TIMEOUT_MS
#define CONFIG_DASHCDG_BADGE_EXEC_LOCK_TIMEOUT_MS 20
#endif

#define BADGE_EXEC_LOCK_TICKS pdMS_TO_TICKS(CONFIG_DASHCDG_BADGE_EXEC_LOCK_TIMEOUT_MS)

/* Bits whose ownership belongs to the orchestrator decision, not to subsystem publishers. */
#define BADGE_EXEC_BOOT_RESERVED_BITS \
    (DASHCDG_BADGE_EXEC_BOOT_COMPLETE_NOMINAL | DASHCDG_BADGE_EXEC_BOOT_COMPLETE_DEGRADED)

typedef struct {
    EventGroupHandle_t boot_evt;
    SemaphoreHandle_t lock;
    dashcdg_badge_exec_health_snapshot_t health[DASHCDG_BADGE_EXEC_SUB__COUNT];
    dashcdg_badge_exec_task_info_t tasks[DASHCDG_BADGE_EXEC_TASK_SLOTS];
    size_t task_count;
    bool initialized;
    bool boot_complete_published;
} badge_exec_state_t;

static badge_exec_state_t s_state;

/* ------------------------------------------------------------------------- */

static bool badge_exec_lock(void)
{
    if (!s_state.initialized || s_state.lock == NULL) {
        return false;
    }
    return xSemaphoreTake(s_state.lock, BADGE_EXEC_LOCK_TICKS) == pdTRUE;
}

static void badge_exec_unlock(void)
{
    if (s_state.lock != NULL) {
        (void)xSemaphoreGive(s_state.lock);
    }
}

uint64_t dashcdg_badge_exec_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------------- */

esp_err_t dashcdg_badge_exec_init(void)
{
    if (s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_state, 0, sizeof(s_state));

    s_state.boot_evt = xEventGroupCreate();
    if (s_state.boot_evt == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_state.lock = xSemaphoreCreateMutex();
    if (s_state.lock == NULL) {
        vEventGroupDelete(s_state.boot_evt);
        s_state.boot_evt = NULL;
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < DASHCDG_BADGE_EXEC_SUB__COUNT; ++i) {
        s_state.health[i].subsystem = (dashcdg_badge_exec_subsystem_t)i;
        s_state.health[i].health = DASHCDG_BADGE_EXEC_HEALTH_UNKNOWN;
        s_state.health[i].since_ms = dashcdg_badge_exec_now_ms();
        s_state.health[i].transitions = 0U;
        s_state.health[i].reason[0] = '\0';
    }

    s_state.initialized = true;
    ESP_LOGI(TAG, "init ok lock_timeout_ms=%d", CONFIG_DASHCDG_BADGE_EXEC_LOCK_TIMEOUT_MS);
    dashcdg_badge_exec_trace("exec_init", "ok lock_timeout_ms=%d",
                             CONFIG_DASHCDG_BADGE_EXEC_LOCK_TIMEOUT_MS);
    return ESP_OK;
}

bool dashcdg_badge_exec_is_initialized(void)
{
    return s_state.initialized;
}

/* ------------------------------------------------------------------------- */
/*  Boot event publication                                                   */
/* ------------------------------------------------------------------------- */

esp_err_t dashcdg_badge_exec_publish_boot_event(uint32_t bits, const char *reason)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bits == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((bits & BADGE_EXEC_BOOT_RESERVED_BITS) != 0U) {
        ESP_LOGW(TAG, "publish_boot_event rejected reserved bits=0x%06" PRIx32, bits);
        return ESP_ERR_INVALID_ARG;
    }

    EventBits_t was = xEventGroupSetBits(s_state.boot_evt, (EventBits_t)bits);
    uint32_t newly = bits & ~(uint32_t)was;
    uint64_t t = dashcdg_badge_exec_now_ms();

    if (newly != 0U) {
        ESP_LOGI(TAG, "boot_evt set=0x%06" PRIx32 " newly=0x%06" PRIx32 " t=%llu reason=%s",
                 bits, newly, (unsigned long long)t,
                 (reason != NULL) ? reason : "");
        dashcdg_badge_exec_trace("boot_evt",
                                 "set=0x%06" PRIx32 " newly=0x%06" PRIx32 " t=%llu reason=%s",
                                 bits, newly, (unsigned long long)t,
                                 (reason != NULL) ? reason : "");
    }
    return ESP_OK;
}

uint32_t dashcdg_badge_exec_get_boot_bits(void)
{
    if (!s_state.initialized) {
        return 0U;
    }
    return (uint32_t)xEventGroupGetBits(s_state.boot_evt);
}

uint32_t dashcdg_badge_exec_wait_boot_any(uint32_t mask, uint32_t timeout_ms)
{
    if (!s_state.initialized || mask == 0U) {
        return 0U;
    }
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_state.boot_evt, (EventBits_t)mask,
                                           pdFALSE, /* clear on exit */
                                           pdFALSE, /* wait for any */
                                           ticks);
    return (uint32_t)bits & mask;
}

bool dashcdg_badge_exec_wait_boot_all(uint32_t mask, uint32_t timeout_ms)
{
    if (!s_state.initialized || mask == 0U) {
        return false;
    }
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_state.boot_evt, (EventBits_t)mask,
                                           pdFALSE, /* clear on exit */
                                           pdTRUE,  /* wait for all */
                                           ticks);
    return (bits & mask) == (EventBits_t)mask;
}

esp_err_t dashcdg_badge_exec_set_boot_complete(bool degraded, const char *reason)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state.boot_complete_published) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t bit = degraded
        ? DASHCDG_BADGE_EXEC_BOOT_COMPLETE_DEGRADED
        : DASHCDG_BADGE_EXEC_BOOT_COMPLETE_NOMINAL;
    s_state.boot_complete_published = true;
    (void)xEventGroupSetBits(s_state.boot_evt, (EventBits_t)bit);

    uint64_t t = dashcdg_badge_exec_now_ms();
    ESP_LOGI(TAG, "boot_complete %s t=%llu reason=%s",
             degraded ? "degraded" : "nominal",
             (unsigned long long)t,
             (reason != NULL) ? reason : "");
    dashcdg_badge_exec_trace("boot_complete", "%s t=%llu reason=%s",
                             degraded ? "degraded" : "nominal",
                             (unsigned long long)t,
                             (reason != NULL) ? reason : "");
    return ESP_OK;
}

int dashcdg_badge_exec_get_boot_complete_state(void)
{
    if (!s_state.initialized) {
        return 0;
    }
    uint32_t bits = (uint32_t)xEventGroupGetBits(s_state.boot_evt);
    if (bits & DASHCDG_BADGE_EXEC_BOOT_COMPLETE_NOMINAL) {
        return 1;
    }
    if (bits & DASHCDG_BADGE_EXEC_BOOT_COMPLETE_DEGRADED) {
        return 2;
    }
    return 0;
}

/*
 * Build a short human-readable reason describing why the boot was DEGRADED. Each contributing bit
 * adds its short tag to a comma-separated list. NOMINAL boots produce "ok".
 */
static void badge_exec_format_boot_reason(uint32_t bits, bool degraded,
                                          char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (!degraded) {
        snprintf(out, out_len, "ok");
        return;
    }
    size_t off = 0;
    out[0] = '\0';

#define BADGE_EXEC_REASON_APPEND(bit_mask, tag) do { \
    if ((bits & (bit_mask)) != 0U && off + 1 < out_len) { \
        int n = snprintf(out + off, out_len - off, "%s%s", (off > 0U) ? "," : "", (tag)); \
        if (n > 0) { off += (size_t)n; } \
    } \
} while (0)

    BADGE_EXEC_REASON_APPEND(DASHCDG_BADGE_EXEC_BOOT_NVS_FATAL, "nvs_fatal");
    BADGE_EXEC_REASON_APPEND(DASHCDG_BADGE_EXEC_BOOT_NVS_RECOVERED, "nvs_recovered");
    BADGE_EXEC_REASON_APPEND(DASHCDG_BADGE_EXEC_BOOT_DISPLAY_FATAL, "display_fatal");
    BADGE_EXEC_REASON_APPEND(DASHCDG_BADGE_EXEC_BOOT_WIFI_NO_CREDS, "wifi_no_creds");
    BADGE_EXEC_REASON_APPEND(DASHCDG_BADGE_EXEC_BOOT_WIFI_DHCP_TIMEOUT, "wifi_dhcp_timeout");
    BADGE_EXEC_REASON_APPEND(DASHCDG_BADGE_EXEC_BOOT_HW_PARTIAL, "hw_partial");
    BADGE_EXEC_REASON_APPEND(DASHCDG_BADGE_EXEC_BOOT_TOUCH_CAL_REQUIRED, "touch_cal_required");
    BADGE_EXEC_REASON_APPEND(DASHCDG_BADGE_EXEC_BOOT_RX_NO_CDG_HEAP, "rx_no_cdg_heap");
    BADGE_EXEC_REASON_APPEND(DASHCDG_BADGE_EXEC_BOOT_DAC_DEGRADED, "dac_degraded");
    BADGE_EXEC_REASON_APPEND(DASHCDG_BADGE_EXEC_BOOT_RX_AUDIO_ONLY_OK, "rx_audio_only");

#undef BADGE_EXEC_REASON_APPEND

    if (off == 0U) {
        snprintf(out, out_len, "degraded");
    }
}

esp_err_t dashcdg_badge_exec_decide_boot_complete(void)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state.boot_complete_published) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t bits = (uint32_t)xEventGroupGetBits(s_state.boot_evt);

    const uint32_t nvs_any = DASHCDG_BADGE_EXEC_BOOT_NVS_OK | DASHCDG_BADGE_EXEC_BOOT_NVS_RECOVERED;
    const uint32_t touch_any = DASHCDG_BADGE_EXEC_BOOT_TOUCH_OK | DASHCDG_BADGE_EXEC_BOOT_TOUCH_CAL_REQUIRED;
    const uint32_t hw_any = DASHCDG_BADGE_EXEC_BOOT_HW_OK | DASHCDG_BADGE_EXEC_BOOT_HW_PARTIAL;

    bool required_ok =
        (bits & nvs_any) != 0U &&
        (bits & DASHCDG_BADGE_EXEC_BOOT_NETIF_OK) != 0U &&
        (bits & DASHCDG_BADGE_EXEC_BOOT_EVT_LOOP_OK) != 0U &&
        (bits & DASHCDG_BADGE_EXEC_BOOT_WIFI_DRV_OK) != 0U &&
        (bits & DASHCDG_BADGE_EXEC_BOOT_DISPLAY_OK) != 0U &&
        (bits & touch_any) != 0U &&
        (bits & hw_any) != 0U;

    /* Forced degraded triggers. */
    const uint32_t degraded_mask =
        DASHCDG_BADGE_EXEC_BOOT_NVS_RECOVERED |
        DASHCDG_BADGE_EXEC_BOOT_NVS_FATAL |
        DASHCDG_BADGE_EXEC_BOOT_DISPLAY_FATAL |
        DASHCDG_BADGE_EXEC_BOOT_WIFI_NO_CREDS |
        DASHCDG_BADGE_EXEC_BOOT_WIFI_DHCP_TIMEOUT |
        DASHCDG_BADGE_EXEC_BOOT_HW_PARTIAL |
        DASHCDG_BADGE_EXEC_BOOT_TOUCH_CAL_REQUIRED |
        DASHCDG_BADGE_EXEC_BOOT_RX_NO_CDG_HEAP |
        DASHCDG_BADGE_EXEC_BOOT_DAC_DEGRADED |
        DASHCDG_BADGE_EXEC_BOOT_RX_AUDIO_ONLY_OK;

    bool degraded = !required_ok || ((bits & degraded_mask) != 0U);

    char reason[80];
    badge_exec_format_boot_reason(bits, degraded, reason, sizeof(reason));
    if (!required_ok) {
        /* Make it obvious which required bit was missing. */
        char missing[64];
        size_t off = 0;
        missing[0] = '\0';
#define BADGE_EXEC_MISSING_APPEND(mask, tag) do { \
    if ((bits & (mask)) == 0U && off + 1 < sizeof(missing)) { \
        int n = snprintf(missing + off, sizeof(missing) - off, "%s%s", (off > 0U) ? "," : "", (tag)); \
        if (n > 0) { off += (size_t)n; } \
    } \
} while (0)
        if ((bits & nvs_any) == 0U) { BADGE_EXEC_MISSING_APPEND(nvs_any, "nvs"); }
        BADGE_EXEC_MISSING_APPEND(DASHCDG_BADGE_EXEC_BOOT_NETIF_OK, "netif");
        BADGE_EXEC_MISSING_APPEND(DASHCDG_BADGE_EXEC_BOOT_EVT_LOOP_OK, "evt_loop");
        BADGE_EXEC_MISSING_APPEND(DASHCDG_BADGE_EXEC_BOOT_WIFI_DRV_OK, "wifi_drv");
        BADGE_EXEC_MISSING_APPEND(DASHCDG_BADGE_EXEC_BOOT_DISPLAY_OK, "display");
        if ((bits & touch_any) == 0U) { BADGE_EXEC_MISSING_APPEND(touch_any, "touch"); }
        if ((bits & hw_any) == 0U) { BADGE_EXEC_MISSING_APPEND(hw_any, "hw"); }
#undef BADGE_EXEC_MISSING_APPEND
        char merged[sizeof(reason)];
        snprintf(merged, sizeof(merged), "missing=%s%s%s",
                 missing,
                 reason[0] ? "," : "",
                 reason[0] ? reason : "");
        return dashcdg_badge_exec_set_boot_complete(true, merged);
    }
    return dashcdg_badge_exec_set_boot_complete(degraded, reason);
}

/* ------------------------------------------------------------------------- */
/*  Health table                                                             */
/* ------------------------------------------------------------------------- */

esp_err_t dashcdg_badge_exec_set_health(dashcdg_badge_exec_subsystem_t sub,
                                        dashcdg_badge_exec_health_t health,
                                        const char *reason)
{
    if (!s_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((unsigned)sub >= (unsigned)DASHCDG_BADGE_EXEC_SUB__COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!badge_exec_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    dashcdg_badge_exec_health_t prev = s_state.health[sub].health;
    bool changed = (prev != health);
    if (changed) {
        s_state.health[sub].transitions++;
        s_state.health[sub].since_ms = dashcdg_badge_exec_now_ms();
        s_state.health[sub].health = health;
    }
    if (reason != NULL) {
        snprintf(s_state.health[sub].reason, sizeof(s_state.health[sub].reason), "%s", reason);
    } else if (changed) {
        s_state.health[sub].reason[0] = '\0';
    }
    badge_exec_unlock();

    if (changed) {
        ESP_LOGI(TAG, "health %s -> %s reason=%s",
                 dashcdg_badge_exec_subsystem_name(sub),
                 dashcdg_badge_exec_health_name(health),
                 (reason != NULL) ? reason : "");
        dashcdg_badge_exec_trace("health", "sub=%s from=%s to=%s reason=%s",
                                 dashcdg_badge_exec_subsystem_name(sub),
                                 dashcdg_badge_exec_health_name(prev),
                                 dashcdg_badge_exec_health_name(health),
                                 (reason != NULL) ? reason : "");
    }
    return ESP_OK;
}

esp_err_t dashcdg_badge_exec_get_health(dashcdg_badge_exec_subsystem_t sub,
                                        dashcdg_badge_exec_health_snapshot_t *out)
{
    if (!s_state.initialized || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((unsigned)sub >= (unsigned)DASHCDG_BADGE_EXEC_SUB__COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!badge_exec_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    *out = s_state.health[sub];
    badge_exec_unlock();
    return ESP_OK;
}

const char *dashcdg_badge_exec_subsystem_name(dashcdg_badge_exec_subsystem_t sub)
{
    switch (sub) {
    case DASHCDG_BADGE_EXEC_SUB_NVS: return "nvs";
    case DASHCDG_BADGE_EXEC_SUB_NETIF: return "netif";
    case DASHCDG_BADGE_EXEC_SUB_EVENT_LOOP: return "evt_loop";
    case DASHCDG_BADGE_EXEC_SUB_WIFI_DRV: return "wifi_drv";
    case DASHCDG_BADGE_EXEC_SUB_WIFI_STA: return "wifi_sta";
    case DASHCDG_BADGE_EXEC_SUB_DISPLAY: return "display";
    case DASHCDG_BADGE_EXEC_SUB_TOUCH: return "touch";
    case DASHCDG_BADGE_EXEC_SUB_PLATFORM_HW: return "platform_hw";
    case DASHCDG_BADGE_EXEC_SUB_VBAT: return "vbat";
    case DASHCDG_BADGE_EXEC_SUB_DAC: return "dac";
    case DASHCDG_BADGE_EXEC_SUB_AUDIO_LAB: return "audio_lab";
    case DASHCDG_BADGE_EXEC_SUB_RX: return "rx";
    case DASHCDG_BADGE_EXEC_SUB_RX_CDG: return "rx_cdg";
    case DASHCDG_BADGE_EXEC_SUB_RX_AUDIO: return "rx_audio";
    case DASHCDG_BADGE_EXEC_SUB_LVGL_UI: return "lvgl_ui";
    case DASHCDG_BADGE_EXEC_SUB__COUNT: break;
    }
    return "unknown";
}

const char *dashcdg_badge_exec_health_name(dashcdg_badge_exec_health_t health)
{
    switch (health) {
    case DASHCDG_BADGE_EXEC_HEALTH_UNKNOWN: return "unknown";
    case DASHCDG_BADGE_EXEC_HEALTH_OK: return "ok";
    case DASHCDG_BADGE_EXEC_HEALTH_DEGRADED: return "degraded";
    case DASHCDG_BADGE_EXEC_HEALTH_TIMEOUT: return "timeout";
    case DASHCDG_BADGE_EXEC_HEALTH_FAILED_OPTIONAL: return "failed_optional";
    case DASHCDG_BADGE_EXEC_HEALTH_FAILED_FATAL: return "failed_fatal";
    }
    return "unknown";
}

/* ------------------------------------------------------------------------- */
/*  Task registry                                                            */
/* ------------------------------------------------------------------------- */

static dashcdg_badge_exec_task_info_t *badge_exec_find_slot_locked(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < DASHCDG_BADGE_EXEC_TASK_SLOTS; ++i) {
        if (s_state.tasks[i].in_use &&
            strncmp(s_state.tasks[i].name, name, DASHCDG_BADGE_EXEC_TASK_NAME_MAX) == 0) {
            return &s_state.tasks[i];
        }
    }
    return NULL;
}

esp_err_t dashcdg_badge_exec_register_task(const char *name,
                                           void *handle,
                                           uint8_t priority,
                                           int8_t core,
                                           uint16_t stack_size)
{
    if (!s_state.initialized || name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (!badge_exec_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    dashcdg_badge_exec_task_info_t *slot = badge_exec_find_slot_locked(name);
    if (slot == NULL) {
        for (size_t i = 0; i < DASHCDG_BADGE_EXEC_TASK_SLOTS; ++i) {
            if (!s_state.tasks[i].in_use) {
                slot = &s_state.tasks[i];
                slot->in_use = 1U;
                s_state.task_count++;
                break;
            }
        }
    }
    if (slot == NULL) {
        badge_exec_unlock();
        ESP_LOGW(TAG, "register_task '%s' rejected: no free slot (max %d)",
                 name, DASHCDG_BADGE_EXEC_TASK_SLOTS);
        return ESP_ERR_NO_MEM;
    }
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->handle = handle;
    slot->priority = priority;
    slot->core = core;
    slot->stack_size = stack_size;
    slot->stack_high_water = 0U;
    slot->last_heartbeat_ms = dashcdg_badge_exec_now_ms();
    slot->last_work_ms = 0U;
    slot->work_count = 0U;
    badge_exec_unlock();

    ESP_LOGI(TAG, "task register '%s' prio=%u core=%d stack=%u",
             slot->name, (unsigned)priority, (int)core, (unsigned)stack_size);
    dashcdg_badge_exec_trace("task_reg", "n=%s p=%u c=%d stk=%u",
                             slot->name, (unsigned)priority, (int)core, (unsigned)stack_size);
    return ESP_OK;
}

esp_err_t dashcdg_badge_exec_unregister_task(const char *name)
{
    if (!s_state.initialized || name == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!badge_exec_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    dashcdg_badge_exec_task_info_t *slot = badge_exec_find_slot_locked(name);
    if (slot != NULL) {
        slot->in_use = 0U;
        slot->handle = NULL;
        slot->name[0] = '\0';
        if (s_state.task_count > 0U) {
            s_state.task_count--;
        }
    }
    badge_exec_unlock();
    return ESP_OK;
}

void dashcdg_badge_exec_task_heartbeat(const char *name)
{
    if (!s_state.initialized || name == NULL) {
        return;
    }
    /*
     * Cheap path: take the lock for the indexed update only. If contended, drop the heartbeat
     * rather than block the caller - WDT policy treats stale heartbeats as a hint, not the only
     * truth source.
     */
    if (xSemaphoreTake(s_state.lock, 0) != pdTRUE) {
        return;
    }
    dashcdg_badge_exec_task_info_t *slot = badge_exec_find_slot_locked(name);
    if (slot != NULL) {
        slot->last_heartbeat_ms = dashcdg_badge_exec_now_ms();
    }
    (void)xSemaphoreGive(s_state.lock);
}

void dashcdg_badge_exec_task_progress(const char *name)
{
    if (!s_state.initialized || name == NULL) {
        return;
    }
    if (xSemaphoreTake(s_state.lock, 0) != pdTRUE) {
        return;
    }
    dashcdg_badge_exec_task_info_t *slot = badge_exec_find_slot_locked(name);
    if (slot != NULL) {
        slot->last_work_ms = dashcdg_badge_exec_now_ms();
        slot->last_heartbeat_ms = slot->last_work_ms;
        slot->work_count++;
    }
    (void)xSemaphoreGive(s_state.lock);
}

void dashcdg_badge_exec_refresh_stack_hwm(void)
{
    if (!s_state.initialized) {
        return;
    }
    if (!badge_exec_lock()) {
        return;
    }
    for (size_t i = 0; i < DASHCDG_BADGE_EXEC_TASK_SLOTS; ++i) {
        dashcdg_badge_exec_task_info_t *slot = &s_state.tasks[i];
        if (!slot->in_use || slot->handle == NULL) {
            continue;
        }
        UBaseType_t free_words = uxTaskGetStackHighWaterMark((TaskHandle_t)slot->handle);
        slot->stack_high_water = (uint32_t)free_words;
    }
    badge_exec_unlock();
}

esp_err_t dashcdg_badge_exec_get_task_info(size_t idx, dashcdg_badge_exec_task_info_t *out)
{
    if (!s_state.initialized || out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!badge_exec_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t r = ESP_ERR_NOT_FOUND;
    size_t seen = 0;
    for (size_t i = 0; i < DASHCDG_BADGE_EXEC_TASK_SLOTS; ++i) {
        if (!s_state.tasks[i].in_use) {
            continue;
        }
        if (seen == idx) {
            *out = s_state.tasks[i];
            r = ESP_OK;
            break;
        }
        seen++;
    }
    badge_exec_unlock();
    return r;
}

size_t dashcdg_badge_exec_get_task_count(void)
{
    if (!s_state.initialized) {
        return 0;
    }
    if (!badge_exec_lock()) {
        return 0;
    }
    size_t n = s_state.task_count;
    badge_exec_unlock();
    return n;
}

/* ------------------------------------------------------------------------- */
/*  Trace                                                                    */
/* ------------------------------------------------------------------------- */

void dashcdg_badge_exec_trace(const char *kind, const char *fmt, ...)
{
#if CONFIG_DASHCDG_BADGE_EXEC_TRACE
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    /*
     * Single-line, structured. The telemetry runbook documents the parser contract; keeping the
     * prefix `[exec-trace] kind=...` makes the lines easy to filter without competing with the
     * rest of ESP_LOG output. ESP_LOGI level is intentional: TRACE level would be filtered out by
     * default builds, and these lines are low-volume by design.
     */
    ESP_LOGI(TAG, "trace kind=%s %s", (kind != NULL) ? kind : "", line);
#else
    (void)kind;
    (void)fmt;
#endif
}
