/*
 * badge_exec - executive layer for the ESP32 badge firmware.
 *
 * Owns:
 *   - Boot event group: each independent boot dependency publishes exactly one forward-progress
 *     fact (OK / DEGRADED / TIMEOUT / FAILED_OPTIONAL / FAILED_FATAL) plus optional "BOOT_COMPLETE_*"
 *     bits decided by the orchestrator.
 *   - Health table: compact per-subsystem health enum plus last reason code/text. Read by UI, WDT
 *     liveness policy, and tests.
 *   - Task registry: name, priority, core, stack high-water snapshot, last heartbeat tick. Used by
 *     the WDT policy and by the telemetry runbook.
 *   - Trace helpers: append-only structured boot/health/liveness lines on UART when enabled. The
 *     telemetry runbook documents the line shapes consumed by scripts/esp32_badge_log_summary.py.
 *
 * Design rules (see docs/specs/esp32-badge-freertos-executive-refactor-spec.md):
 *   - All APIs are non-blocking enough to call from owner tasks at loop boundaries.
 *   - No API blocks while holding another executive lock.
 *   - Audio decode and video decode are independent. The boot decision must reach
 *     BOOT_COMPLETE_DEGRADED rather than failing when only one of the two media paths is healthy.
 *   - No API performs UI/LVGL/NVS work directly.
 *
 * This header is safe to include in main.c, ESP event handlers, and owner tasks. ISR callers must
 * use only the dashcdg_badge_exec_*_isr suffixed entry points where provided.
 */

#ifndef DASHCDG_BADGE_EXEC_H
#define DASHCDG_BADGE_EXEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/*  Boot event bits                                                          */
/* ------------------------------------------------------------------------- */
/*
 * Boot bits are packed in a single FreeRTOS event group (24 user bits available). Each independent
 * dependency contributes a small mutually exclusive group: the orchestrator only acts when at least
 * one bit per required dependency is set, choosing nominal vs degraded vs fatal from the union.
 *
 * Hard rule: a bit is latched. Once set it must not be cleared except by an explicit recovery path
 * documented in the executive refactor spec.
 */

/* NVS / event-loop / netif (core infra). */
#define DASHCDG_BADGE_EXEC_BOOT_NVS_OK              (1U << 0)
#define DASHCDG_BADGE_EXEC_BOOT_NVS_RECOVERED       (1U << 1) /* erase+reinit succeeded */
#define DASHCDG_BADGE_EXEC_BOOT_NVS_FATAL           (1U << 2)
#define DASHCDG_BADGE_EXEC_BOOT_NETIF_OK            (1U << 3)
#define DASHCDG_BADGE_EXEC_BOOT_EVT_LOOP_OK         (1U << 4)

/* Wi-Fi driver + STA progress. */
#define DASHCDG_BADGE_EXEC_BOOT_WIFI_DRV_OK         (1U << 5)
#define DASHCDG_BADGE_EXEC_BOOT_WIFI_NO_CREDS       (1U << 6)
#define DASHCDG_BADGE_EXEC_BOOT_WIFI_CONNECTING     (1U << 7)
#define DASHCDG_BADGE_EXEC_BOOT_WIFI_GOT_IP         (1U << 8)
#define DASHCDG_BADGE_EXEC_BOOT_WIFI_DHCP_TIMEOUT   (1U << 9)

/* Display / touch / platform HW. */
#define DASHCDG_BADGE_EXEC_BOOT_DISPLAY_OK          (1U << 10)
#define DASHCDG_BADGE_EXEC_BOOT_DISPLAY_FATAL       (1U << 11)
#define DASHCDG_BADGE_EXEC_BOOT_TOUCH_OK            (1U << 12)
#define DASHCDG_BADGE_EXEC_BOOT_TOUCH_CAL_REQUIRED  (1U << 13)
#define DASHCDG_BADGE_EXEC_BOOT_HW_OK               (1U << 14)
#define DASHCDG_BADGE_EXEC_BOOT_HW_PARTIAL          (1U << 15)
#define DASHCDG_BADGE_EXEC_BOOT_VBAT_OK             (1U << 16)

/* RX + media resources. */
#define DASHCDG_BADGE_EXEC_BOOT_RX_RESOURCES_OK     (1U << 17)
#define DASHCDG_BADGE_EXEC_BOOT_RX_NO_CDG_HEAP      (1U << 18)
#define DASHCDG_BADGE_EXEC_BOOT_RX_AUDIO_ONLY_OK    (1U << 19)
#define DASHCDG_BADGE_EXEC_BOOT_DAC_OK              (1U << 20)
#define DASHCDG_BADGE_EXEC_BOOT_DAC_DEGRADED        (1U << 21)

/* Orchestrator decisions (set by badge_exec; consumed by readiness / tests). */
#define DASHCDG_BADGE_EXEC_BOOT_COMPLETE_NOMINAL    (1U << 22)
#define DASHCDG_BADGE_EXEC_BOOT_COMPLETE_DEGRADED   (1U << 23)

/* Convenience masks. */
#define DASHCDG_BADGE_EXEC_BOOT_COMPLETE_ANY \
    (DASHCDG_BADGE_EXEC_BOOT_COMPLETE_NOMINAL | DASHCDG_BADGE_EXEC_BOOT_COMPLETE_DEGRADED)

/* ------------------------------------------------------------------------- */
/*  Subsystem health table                                                   */
/* ------------------------------------------------------------------------- */

typedef enum {
    DASHCDG_BADGE_EXEC_SUB_NVS = 0,
    DASHCDG_BADGE_EXEC_SUB_NETIF,
    DASHCDG_BADGE_EXEC_SUB_EVENT_LOOP,
    DASHCDG_BADGE_EXEC_SUB_WIFI_DRV,
    DASHCDG_BADGE_EXEC_SUB_WIFI_STA,
    DASHCDG_BADGE_EXEC_SUB_DISPLAY,
    DASHCDG_BADGE_EXEC_SUB_TOUCH,
    DASHCDG_BADGE_EXEC_SUB_PLATFORM_HW,
    DASHCDG_BADGE_EXEC_SUB_VBAT,
    DASHCDG_BADGE_EXEC_SUB_DAC,
    DASHCDG_BADGE_EXEC_SUB_AUDIO_LAB,
    DASHCDG_BADGE_EXEC_SUB_RX,
    DASHCDG_BADGE_EXEC_SUB_RX_CDG,
    DASHCDG_BADGE_EXEC_SUB_RX_AUDIO,
    DASHCDG_BADGE_EXEC_SUB_LVGL_UI,
    DASHCDG_BADGE_EXEC_SUB__COUNT
} dashcdg_badge_exec_subsystem_t;

typedef enum {
    DASHCDG_BADGE_EXEC_HEALTH_UNKNOWN = 0,
    DASHCDG_BADGE_EXEC_HEALTH_OK,
    DASHCDG_BADGE_EXEC_HEALTH_DEGRADED,
    DASHCDG_BADGE_EXEC_HEALTH_TIMEOUT,
    DASHCDG_BADGE_EXEC_HEALTH_FAILED_OPTIONAL,
    DASHCDG_BADGE_EXEC_HEALTH_FAILED_FATAL
} dashcdg_badge_exec_health_t;

#define DASHCDG_BADGE_EXEC_REASON_MAX 32

typedef struct {
    dashcdg_badge_exec_subsystem_t subsystem;
    dashcdg_badge_exec_health_t health;
    uint64_t since_ms;
    uint32_t transitions;
    char reason[DASHCDG_BADGE_EXEC_REASON_MAX];
} dashcdg_badge_exec_health_snapshot_t;

/* ------------------------------------------------------------------------- */
/*  Task registry                                                            */
/* ------------------------------------------------------------------------- */

#define DASHCDG_BADGE_EXEC_TASK_NAME_MAX 16
#define DASHCDG_BADGE_EXEC_TASK_SLOTS    12

typedef struct {
    char name[DASHCDG_BADGE_EXEC_TASK_NAME_MAX];
    void *handle;             /* FreeRTOS TaskHandle_t opaque pointer. */
    uint8_t priority;
    int8_t core;              /* -1 = no affinity */
    uint16_t stack_size;
    uint32_t stack_high_water; /* words; refreshed via dashcdg_badge_exec_refresh_stack_hwm */
    uint64_t last_heartbeat_ms;
    uint64_t last_work_ms;
    uint32_t work_count;
    uint8_t in_use;
} dashcdg_badge_exec_task_info_t;

/* ------------------------------------------------------------------------- */
/*  Lifecycle                                                                */
/* ------------------------------------------------------------------------- */

/**
 * Create internal state. Safe to call once at boot, before any publisher runs.
 *
 * Returns ESP_OK on success, ESP_ERR_NO_MEM if FreeRTOS primitives could not be created,
 * ESP_ERR_INVALID_STATE on double-init.
 */
esp_err_t dashcdg_badge_exec_init(void);

/**
 * True after dashcdg_badge_exec_init succeeded. Publishers may early-return when this is false to
 * tolerate test harnesses that exercise individual subsystems in isolation.
 */
bool dashcdg_badge_exec_is_initialized(void);

/* ------------------------------------------------------------------------- */
/*  Boot event publication / observation                                     */
/* ------------------------------------------------------------------------- */

/**
 * Atomically set one or more BOOT_* bits and emit a trace line. Bits must not overlap orchestrator
 * decision bits (BOOT_COMPLETE_*); use dashcdg_badge_exec_set_boot_complete for those.
 */
esp_err_t dashcdg_badge_exec_publish_boot_event(uint32_t bits, const char *reason);

/**
 * Read the latched boot bits.
 */
uint32_t dashcdg_badge_exec_get_boot_bits(void);

/**
 * Wait until any bit in mask is set, up to timeout_ms. Returns set bits (mask of bits at wake).
 * Pass timeout_ms = 0 for non-blocking poll. Pass UINT32_MAX for "wait forever" - waivable use only.
 */
uint32_t dashcdg_badge_exec_wait_boot_any(uint32_t mask, uint32_t timeout_ms);

/**
 * Wait until all bits in mask are set, up to timeout_ms. Returns true on full match.
 */
bool dashcdg_badge_exec_wait_boot_all(uint32_t mask, uint32_t timeout_ms);

/**
 * Orchestrator decision: set BOOT_COMPLETE_NOMINAL or BOOT_COMPLETE_DEGRADED. Mutually exclusive.
 * Idempotent; first decision wins.
 */
esp_err_t dashcdg_badge_exec_set_boot_complete(bool degraded, const char *reason);

/**
 * Run the default boot-complete decision: look at currently latched boot bits and publish
 * BOOT_COMPLETE_NOMINAL when every required dependency reports OK and no degraded triggers are
 * latched; otherwise publish BOOT_COMPLETE_DEGRADED with a reason that lists the contributing
 * bits. Idempotent (subsequent calls return ESP_ERR_INVALID_STATE).
 *
 * Decision rules (see docs/specs/esp32-badge-freertos-executive-refactor-spec.md section 4):
 *   - Required for NOMINAL:
 *       (NVS_OK | NVS_RECOVERED) & NETIF_OK & EVT_LOOP_OK & WIFI_DRV_OK & DISPLAY_OK &
 *       (TOUCH_OK | TOUCH_CAL_REQUIRED) & (HW_OK | HW_PARTIAL).
 *   - Any of NVS_RECOVERED, WIFI_NO_CREDS, WIFI_DHCP_TIMEOUT, HW_PARTIAL, TOUCH_CAL_REQUIRED,
 *     RX_NO_CDG_HEAP, DAC_DEGRADED, RX_AUDIO_ONLY_OK forces DEGRADED.
 *   - DISPLAY_FATAL or NVS_FATAL would have already aborted boot; if either is still set the
 *     decision falls through to DEGRADED with a fatal reason string (the caller should not be
 *     proceeding in that case).
 *   - Audio/video independence: RX_NO_CDG_HEAP and DAC_DEGRADED produce DEGRADED but never FATAL.
 */
esp_err_t dashcdg_badge_exec_decide_boot_complete(void);

/**
 * 0 = not yet decided, 1 = nominal, 2 = degraded.
 */
int dashcdg_badge_exec_get_boot_complete_state(void);

/* ------------------------------------------------------------------------- */
/*  Health table                                                             */
/* ------------------------------------------------------------------------- */

/**
 * Record subsystem health. `reason` may be NULL. Health transitions are counted and logged.
 */
esp_err_t dashcdg_badge_exec_set_health(dashcdg_badge_exec_subsystem_t sub,
                                        dashcdg_badge_exec_health_t health,
                                        const char *reason);

/**
 * Copy out a snapshot of one subsystem's current state.
 */
esp_err_t dashcdg_badge_exec_get_health(dashcdg_badge_exec_subsystem_t sub,
                                        dashcdg_badge_exec_health_snapshot_t *out);

/**
 * Human-readable subsystem / health label. Returns short ASCII strings safe to ESP_LOGI.
 */
const char *dashcdg_badge_exec_subsystem_name(dashcdg_badge_exec_subsystem_t sub);
const char *dashcdg_badge_exec_health_name(dashcdg_badge_exec_health_t health);

/* ------------------------------------------------------------------------- */
/*  Task registry                                                            */
/* ------------------------------------------------------------------------- */

/**
 * Register a FreeRTOS task with the executive. `handle` should be the value returned by
 * xTaskCreate / xTaskCreatePinnedToCore. Pass core = -1 for no affinity.
 *
 * Returns ESP_ERR_NO_MEM if no slot is free (compile-time DASHCDG_BADGE_EXEC_TASK_SLOTS).
 */
esp_err_t dashcdg_badge_exec_register_task(const char *name,
                                           void *handle,
                                           uint8_t priority,
                                           int8_t core,
                                           uint16_t stack_size);

/**
 * Remove a task from the registry (e.g. teardown path). Idempotent.
 */
esp_err_t dashcdg_badge_exec_unregister_task(const char *name);

/**
 * Mark task `name` as alive (loop boundary heartbeat). Cheap; safe to call frequently.
 */
void dashcdg_badge_exec_task_heartbeat(const char *name);

/**
 * Mark task `name` as having made forward progress (e.g. dispatched a packet, drew a frame).
 * Separate from `_heartbeat` so the WDT policy can distinguish "alive idle" from "alive making
 * progress" once T7 enables enforcement.
 */
void dashcdg_badge_exec_task_progress(const char *name);

/**
 * Refresh stack high-water marks for all registered tasks. Cheap on ESP-IDF; may be called from a
 * low-rate housekeeping context.
 */
void dashcdg_badge_exec_refresh_stack_hwm(void);

/**
 * Copy out one slot from the task registry. Returns ESP_ERR_NOT_FOUND when idx >= registered count.
 */
esp_err_t dashcdg_badge_exec_get_task_info(size_t idx, dashcdg_badge_exec_task_info_t *out);

/**
 * Number of currently registered tasks.
 */
size_t dashcdg_badge_exec_get_task_count(void);

/* ------------------------------------------------------------------------- */
/*  Liveness sweep (T7)                                                      */
/* ------------------------------------------------------------------------- */

/**
 * Start the periodic liveness sweep. The sweep runs from an esp_timer (no dedicated task) and
 * performs only registry walks: refresh stack high-water marks, compute heartbeat lag, emit a
 * [exec-trace] line per stalled task, and update the global stall counters.
 *
 * Per the T7 design contract this is observe-only by default (CONFIG_DASHCDG_BADGE_EXEC_LIVENESS_
 * ENFORCE = n). When enforce is set, the sweep ALSO calls dashcdg_badge_exec_set_health(...
 * DEGRADED, "no_heartbeat") for the subsystem that owns each stalled task (mapping defined in
 * badge_exec.c). The sweep never reboots the badge or calls into the WDT directly.
 *
 * Returns ESP_OK on first call, ESP_ERR_INVALID_STATE if already running or exec not initialized.
 */
esp_err_t dashcdg_badge_exec_liveness_start(void);

/**
 * Stop the periodic liveness sweep. Used by tests and shutdown paths. Idempotent.
 */
void dashcdg_badge_exec_liveness_stop(void);

typedef struct {
    uint32_t sweeps_total;        /* incremented every sweep tick */
    uint32_t stalls_observed;     /* per-task stall observations (one per stalled task per sweep) */
    uint32_t enforce_transitions; /* health transitions made by the sweep when enforce is on */
    uint32_t worst_lag_ms;        /* peak (now - last_heartbeat_ms) seen across all tasks */
} dashcdg_badge_exec_liveness_stats_t;

void dashcdg_badge_exec_liveness_get_stats(dashcdg_badge_exec_liveness_stats_t *out);

/* ------------------------------------------------------------------------- */
/*  LVGL tick budget observation (T8)                                        */
/* ------------------------------------------------------------------------- */

/*
 * LVGL timer callbacks share a single LVGL task. A callback that exceeds its scheduled period
 * starves all other timers in the same task and is the dominant root cause of UI tearing /
 * "frozen knobs" reports during bring-up. T8 introduces an observe-only contract:
 *
 *   1. Each high-frequency UI tick measures its wall duration with esp_timer.
 *   2. It calls dashcdg_badge_exec_ui_tick_observe(name, dur_us) at the end of the tick.
 *   3. The executive maintains a small bounded table (one slot per unique name) recording
 *      last duration, max duration, total tick count, and over-budget count.
 *   4. When dur_us exceeds CONFIG_DASHCDG_BADGE_UI_TICK_OVERRUN_US, the executive emits a
 *      throttled [exec-trace] line (one log per name every 5 s) and increments the overrun
 *      counter. No automatic reboot, no health transition - this is the data layer that the
 *      readiness checklist verifies before enforcement is enabled.
 *
 * Safe to call from any non-ISR context. Names are matched as 0-terminated strings up to
 * DASHCDG_BADGE_EXEC_TASK_NAME_MAX bytes; longer names are truncated. Table is bounded by
 * DASHCDG_BADGE_EXEC_UI_TICK_SLOTS; further unique names are dropped (drop counter exposed).
 */

#define DASHCDG_BADGE_EXEC_UI_TICK_SLOTS 8

typedef struct {
    char name[DASHCDG_BADGE_EXEC_TASK_NAME_MAX];
    uint32_t ticks;          /* total calls to _ui_tick_observe with this name */
    uint32_t overruns;       /* dur_us > CONFIG_DASHCDG_BADGE_UI_TICK_OVERRUN_US */
    uint32_t last_us;        /* most recent duration */
    uint32_t max_us;         /* peak duration since boot */
    uint8_t in_use;
} dashcdg_badge_exec_ui_tick_info_t;

void dashcdg_badge_exec_ui_tick_observe(const char *name, uint32_t dur_us);

esp_err_t dashcdg_badge_exec_ui_tick_get_info(size_t idx,
                                              dashcdg_badge_exec_ui_tick_info_t *out);

size_t dashcdg_badge_exec_ui_tick_get_count(void);

/*
 * Number of unique tick names dropped because DASHCDG_BADGE_EXEC_UI_TICK_SLOTS was exhausted.
 */
uint32_t dashcdg_badge_exec_ui_tick_get_dropped(void);

/* ------------------------------------------------------------------------- */
/*  Timing                                                                   */
/* ------------------------------------------------------------------------- */

/**
 * Monotonic milliseconds since boot. Wraps the ESP-IDF esp_timer microsecond clock.
 */
uint64_t dashcdg_badge_exec_now_ms(void);

/* ------------------------------------------------------------------------- */
/*  Trace                                                                    */
/* ------------------------------------------------------------------------- */

/**
 * Emit a structured trace line. Cheap when CONFIG_DASHCDG_BADGE_EXEC_TRACE is disabled (no-op).
 *
 * Format follows docs/ops/esp32-badge-freertos-telemetry-runbook.md UART fallback shapes.
 */
void dashcdg_badge_exec_trace(const char *kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif /* DASHCDG_BADGE_EXEC_H */
