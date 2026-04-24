/*
 * Low-priority FreeRTOS task: RGB status LED (LEDC), backlight PWM (IO27), battery cache,
 * user button (IO0), SC8002B enable (IO4), PWM beep on IO26, external I2C init (IO32/IO25).
 */
#include "platform_hw.h"

#include "board_badge_hw.h"
#include "board_cyd_freenove_32.h"
#include "badge_prefs.h"

#include "dashcdg/media_clock.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "vbat_sense.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "platform_hw";

#define HW_TASK_STACK  3072
#define HW_TASK_PRIO   1
#define HW_TICK_MS     40

#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_TIMER_RGB_BL   LEDC_TIMER_0
#define LEDC_TIMER_BEEP     LEDC_TIMER_1
#define LEDC_CH_R           LEDC_CHANNEL_0
#define LEDC_CH_G           LEDC_CHANNEL_1
#define LEDC_CH_B           LEDC_CHANNEL_2
#define LEDC_CH_BL          LEDC_CHANNEL_3
#define LEDC_CH_AUDIO       LEDC_CHANNEL_4

#define LEDC_DUTY_RES       LEDC_TIMER_8_BIT
#define LEDC_RGB_BL_HZ      5000
#define LEDC_BEEP_HZ        1760

#define BEEP_MS             55

typedef enum {
    PM_ACTIVE = 0,
    PM_DIM,
    PM_SLEEP_FADE, /* backlight ramp to 0, panel still on */
    PM_SLEEP,
    PM_WAKE_FADE, /* panel on, backlight 0 -> user */
} pm_state_t;

static TaskHandle_t s_hw_task;
static SemaphoreHandle_t s_mtx;

static bool s_ready;
static dashcdg_hw_screen_t s_screen;
static bool s_cdg_stream_ok;

/** User-selected max brightness (NVS / settings). */
static uint8_t s_bl_user_pct = 100;
/** Current backlight applied (may be ramped down for idle dim / zero in sleep). */
static uint8_t s_bl_applied_pct = 100;

static uint64_t s_activity_ms;
static pm_state_t s_pm_state;
static volatile int32_t s_disp_pwrcmd;

static bool s_wifi_ps_saved_valid;
static wifi_ps_type_t s_wifi_ps_saved;

static bool s_rgb_status_enabled = true;
static uint8_t s_rgb_status_pct = 100;
static bool s_auto_sleep_enabled = true;

static int s_bat_raw;
static int s_bat_pin_mv;
static int s_bat_vbat_mv;
static uint64_t s_bat_sample_ms;

static uint64_t s_beep_until_ms;
/** 5-100: scales LEDC duty on IO26 (default 85; was hardcoded ~50% peak). */
static uint8_t s_beep_vol_pct = 85;
/** LVGL touch click beep only (not IO0 power tones). */
static bool s_touch_beep_on = true;

static uint8_t s_btn_low_streak;
/** After sleep or wake from IO0, ignore further button edges until GPIO0 is released (avoids sleep->wake loop while held). */
static bool s_btn_block_until_hi;

#define USER_BTN_WAKE_FRAMES   5U   /* ~200 ms @ 40 ms/tick */
#define USER_BTN_SLEEP_FRAMES  35U  /* ~1.4 s hold to sleep */

/** Sleep fade from IO0 long-hold: faster ramp to black than idle auto-sleep. */
static bool s_bl_sleep_fade_manual;

static bool pm_idle_eligible_locked(void)
{
    if (s_screen == DASHCDG_HW_SCREEN_HOME) {
        return true;
    }
    if (s_screen == DASHCDG_HW_SCREEN_KARAOKE && !s_cdg_stream_ok) {
        return true;
    }
    return false;
}

static bool pm_idle_eligible_effective_locked(void)
{
    if (!s_auto_sleep_enabled) {
        return false;
    }
    return pm_idle_eligible_locked();
}

static void pm_force_active_restore_locked(void)
{
    if (s_pm_state == PM_SLEEP) {
        __atomic_store_n(&s_disp_pwrcmd, 2, __ATOMIC_SEQ_CST);
        if (s_wifi_ps_saved_valid) {
            (void)esp_wifi_set_ps(s_wifi_ps_saved);
            s_wifi_ps_saved_valid = false;
        }
    }
    s_bl_sleep_fade_manual = false;
    s_pm_state = PM_ACTIVE;
    s_bl_applied_pct = s_bl_user_pct;
}

static void pm_commit_panel_sleep_locked(void)
{
    s_bl_sleep_fade_manual = false;
    __atomic_store_n(&s_disp_pwrcmd, 1, __ATOMIC_SEQ_CST);
    wifi_ps_type_t cur = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&cur) == ESP_OK) {
        s_wifi_ps_saved = cur;
        s_wifi_ps_saved_valid = true;
        (void)esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    }
    s_pm_state = PM_SLEEP;
    s_bl_applied_pct = 0;
    s_btn_block_until_hi = true;
}

static void pm_request_manual_sleep_fade_locked(void)
{
    if (s_pm_state == PM_SLEEP || s_pm_state == PM_SLEEP_FADE) {
        return;
    }
    s_bl_sleep_fade_manual = true;
    s_pm_state = PM_SLEEP_FADE;
}

static uint8_t pm_idle_dim_target_pct_locked(void)
{
    uint8_t dim_tgt = (uint8_t)((s_bl_user_pct * (uint32_t)DASHCDG_HW_IDLE_DIM_PCT_OF_MAX) / 100U);
    if (dim_tgt < (uint8_t)DASHCDG_HW_IDLE_DIM_MIN_PCT) {
        dim_tgt = (uint8_t)DASHCDG_HW_IDLE_DIM_MIN_PCT;
    }
    return dim_tgt;
}

static void pm_backlight_fade_step_locked(void)
{
    if (s_pm_state == PM_SLEEP_FADE) {
        uint8_t p = s_bl_applied_pct;
        if (p > 0U) {
            uint8_t step;
            if (s_bl_sleep_fade_manual) {
                /* Button sleep: brisk ramp from any level down to off. */
                step = (uint8_t)((p + 5U) / 6U);
            } else {
                /* Idle auto-sleep: slower above idle-dim floor, quicker last miles to black. */
                uint8_t dim_floor = pm_idle_dim_target_pct_locked();
                if (p > (uint8_t)(dim_floor + 2U)) {
                    step = (uint8_t)((p + 24U) / 25U);
                } else {
                    step = (uint8_t)((p + 3U) / 4U);
                }
            }
            if (step < 1U) {
                step = 1U;
            }
            if (p > step) {
                s_bl_applied_pct = (uint8_t)(p - step);
            } else {
                s_bl_applied_pct = 0;
            }
        }
        if (s_bl_applied_pct == 0U) {
            pm_commit_panel_sleep_locked();
        }
        return;
    }
    if (s_pm_state == PM_WAKE_FADE) {
        uint8_t tgt = s_bl_user_pct;
        uint8_t p = s_bl_applied_pct;
        if (p < tgt) {
            uint32_t delta = (uint32_t)tgt - (uint32_t)p;
            /* Slower than idle 100->dim: smooth "device waking" ramp. */
            uint8_t step = (uint8_t)((delta + 47U) / 48U);
            if (step < 1U) {
                step = 1U;
            }
            if ((uint32_t)p + (uint32_t)step > (uint32_t)tgt) {
                s_bl_applied_pct = tgt;
            } else {
                s_bl_applied_pct = (uint8_t)(p + step);
            }
        }
        if (s_bl_applied_pct >= tgt) {
            s_pm_state = PM_ACTIVE;
        }
    }
}

static void pm_bump_activity_locked(uint64_t now)
{
    s_activity_ms = now;
    if (s_pm_state == PM_SLEEP) {
        __atomic_store_n(&s_disp_pwrcmd, 2, __ATOMIC_SEQ_CST);
        s_pm_state = PM_WAKE_FADE;
        s_bl_applied_pct = 0;
        if (s_wifi_ps_saved_valid) {
            (void)esp_wifi_set_ps(s_wifi_ps_saved);
            s_wifi_ps_saved_valid = false;
        }
        s_btn_block_until_hi = true;
    } else if (s_pm_state == PM_SLEEP_FADE) {
        s_bl_sleep_fade_manual = false;
        s_pm_state = PM_ACTIVE;
        s_bl_applied_pct = s_bl_user_pct;
    } else if (s_pm_state == PM_DIM) {
        s_pm_state = PM_ACTIVE;
        s_bl_applied_pct = s_bl_user_pct;
    }
}

static void pm_update_idle_locked(uint64_t now)
{
    if (!pm_idle_eligible_effective_locked()) {
        if (s_pm_state == PM_SLEEP) {
            __atomic_store_n(&s_disp_pwrcmd, 2, __ATOMIC_SEQ_CST);
            if (s_wifi_ps_saved_valid) {
                (void)esp_wifi_set_ps(s_wifi_ps_saved);
                s_wifi_ps_saved_valid = false;
            }
        }
        s_bl_sleep_fade_manual = false;
        s_pm_state = PM_ACTIVE;
        s_bl_applied_pct = s_bl_user_pct;
        return;
    }

    uint64_t idle = (now > s_activity_ms) ? (now - s_activity_ms) : 0U;

    if (idle >= (uint64_t)DASHCDG_HW_IDLE_SLEEP_MS && s_pm_state != PM_SLEEP && s_pm_state != PM_SLEEP_FADE &&
        s_pm_state != PM_WAKE_FADE) {
        s_bl_sleep_fade_manual = false;
        s_pm_state = PM_SLEEP_FADE;
        return;
    }

    if (idle >= (uint64_t)DASHCDG_HW_IDLE_DIM_MS && s_pm_state == PM_ACTIVE) {
        s_pm_state = PM_DIM;
    }

    if (s_pm_state == PM_DIM) {
        uint8_t dim_tgt = pm_idle_dim_target_pct_locked();
        if (s_bl_applied_pct > dim_tgt + 1U) {
            uint32_t div = (uint32_t)DASHCDG_HW_IDLE_DIM_RAMP_DIV;
            if (div < 1U) {
                div = 1U;
            }
            uint8_t step = (uint8_t)((s_bl_applied_pct - dim_tgt) / div);
            if (step < 1U) {
                step = 1U;
            }
            s_bl_applied_pct -= step;
        }
    }
}

static void rgb_set_u8(uint8_t r, uint8_t g, uint8_t b)
{
    /* GPIOs are active-low: higher LEDC duty => more time high => dimmer LED. */
    const uint32_t max_d = ((1U << 8) - 1U);
    uint32_t dr = max_d - (uint32_t)r;
    uint32_t dg = max_d - (uint32_t)g;
    uint32_t db = max_d - (uint32_t)b;
    if (dr > max_d) {
        dr = max_d;
    }
    if (dg > max_d) {
        dg = max_d;
    }
    if (db > max_d) {
        db = max_d;
    }
    ledc_set_duty(LEDC_MODE, LEDC_CH_R, dr);
    ledc_set_duty(LEDC_MODE, LEDC_CH_G, dg);
    ledc_set_duty(LEDC_MODE, LEDC_CH_B, db);
    ledc_update_duty(LEDC_MODE, LEDC_CH_R);
    ledc_update_duty(LEDC_MODE, LEDC_CH_G);
    ledc_update_duty(LEDC_MODE, LEDC_CH_B);
}

static void rgb_apply_status(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_rgb_status_enabled) {
        rgb_set_u8(0, 0, 0);
        return;
    }
    uint32_t sc = (uint32_t)s_rgb_status_pct;
    if (sc > 100U) {
        sc = 100U;
    }
    r = (uint8_t)(((uint32_t)r * sc) / 100U);
    g = (uint8_t)(((uint32_t)g * sc) / 100U);
    b = (uint8_t)(((uint32_t)b * sc) / 100U);
    rgb_set_u8(r, g, b);
}

static void bl_set_pct(uint8_t pct)
{
    if (pct > 100U) {
        pct = 100U;
    }
    uint32_t duty = (uint32_t)pct * 255U / 100U;
    ledc_set_duty(LEDC_MODE, LEDC_CH_BL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CH_BL);
}

static void amp_set_run(bool run)
{
    /* SC8002B: VDD on shutdown = shutdown. LOW = amp on. */
    gpio_set_level(DASHCDG_HW_GPIO_AMP_SHUTDOWN, run ? 0 : 1);
}

static void beep_pwm_off(void)
{
    ledc_set_duty(LEDC_MODE, LEDC_CH_AUDIO, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CH_AUDIO);
    amp_set_run(false);
}

static void beep_pwm_on(void)
{
    amp_set_run(true);
    ledc_set_freq(LEDC_MODE, LEDC_TIMER_BEEP, LEDC_BEEP_HZ);
    uint32_t d = (uint32_t)s_beep_vol_pct * 255U / 100U;
    if (d < 1U) {
        d = 1U;
    }
    if (d > 255U) {
        d = 255U;
    }
    ledc_set_duty(LEDC_MODE, LEDC_CH_AUDIO, d);
    ledc_update_duty(LEDC_MODE, LEDC_CH_AUDIO);
}

static esp_err_t ledc_install_rgb_bl(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER_RGB_BL,
        .freq_hz = LEDC_RGB_BL_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc_timer RGB/BL");

    const gpio_num_t pins[] = {DASHCDG_HW_GPIO_RGB_R, DASHCDG_HW_GPIO_RGB_G, DASHCDG_HW_GPIO_RGB_B, DASHCDG_HW_GPIO_LCD_BL_PWM};
    const ledc_channel_t chans[] = {LEDC_CH_R, LEDC_CH_G, LEDC_CH_B, LEDC_CH_BL};
    for (unsigned i = 0; i < 4; i++) {
        ledc_channel_config_t ch = {0};
        ch.gpio_num = pins[i];
        ch.speed_mode = LEDC_MODE;
        ch.channel = chans[i];
        ch.intr_type = LEDC_INTR_DISABLE;
        ch.timer_sel = LEDC_TIMER_RGB_BL;
        ch.duty = (i < 3) ? 255 : 0;
        ch.hpoint = 0;
        ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc_channel");
    }
    return ESP_OK;
}

static esp_err_t ledc_install_beep(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER_BEEP,
        .freq_hz = LEDC_BEEP_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&t), TAG, "ledc_timer beep");

    ledc_channel_config_t ch = {0};
    ch.gpio_num = DASHCDG_HW_GPIO_AUDIO_PWM;
    ch.speed_mode = LEDC_MODE;
    ch.channel = LEDC_CH_AUDIO;
    ch.intr_type = LEDC_INTR_DISABLE;
    ch.timer_sel = LEDC_TIMER_BEEP;
    ch.duty = 0;
    ch.hpoint = 0;
    return ledc_channel_config(&ch);
}

static esp_err_t gpio_misc_init(void)
{
    gpio_config_t amp = {
        .pin_bit_mask = 1ULL << DASHCDG_HW_GPIO_AMP_SHUTDOWN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&amp), TAG, "gpio amp");
    /* Boot muted: shutdown asserted (HIGH). External 10k may already bias this; drive explicitly. */
    gpio_set_level(DASHCDG_HW_GPIO_AMP_SHUTDOWN, 1);

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << DASHCDG_HW_GPIO_USER_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ONLY,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&btn), TAG, "gpio btn");
    return ESP_OK;
}

static esp_err_t i2c_bus_init(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = DASHCDG_HW_GPIO_I2C_SDA,
        .scl_io_num = DASHCDG_HW_GPIO_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = DASHCDG_HW_I2C_FREQ_HZ,
    };
    esp_err_t e = i2c_param_config(DASHCDG_HW_I2C_PORT, &cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "i2c_param_config: %s", esp_err_to_name(e));
        return e;
    }
    e = i2c_driver_install(DASHCDG_HW_I2C_PORT, cfg.mode, 0, 0, 0);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "i2c_driver_install: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGI(TAG, "I2C%u ready (SDA IO%u SCL IO%u)", (unsigned)DASHCDG_HW_I2C_PORT, (unsigned)DASHCDG_HW_GPIO_I2C_SDA,
             (unsigned)DASHCDG_HW_GPIO_I2C_SCL);
    return ESP_OK;
}

static void sample_battery_update_cache(void)
{
    if (!dashcdg_vbat_sense_is_ready()) {
        return;
    }
    int raw = 0;
    int pin = 0;
    int vbat = 0;
    if (dashcdg_vbat_sense_read(&raw, &pin, &vbat) != ESP_OK) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_bat_raw = raw;
        s_bat_pin_mv = pin;
        s_bat_vbat_mv = vbat;
        s_bat_sample_ms = dashcdg_clock_now_ms();
        xSemaphoreGive(s_mtx);
    }
}

static void led_anim_frame(uint64_t now_ms)
{
    if (s_pm_state == PM_SLEEP) {
        /* Deep standby: very dim slow blue "breath" so the badge still feels alive. */
        if (!s_rgb_status_enabled) {
            rgb_set_u8(0, 0, 0);
            return;
        }
        float t = (float)(now_ms % 7000ULL) * (2.0f * (float)M_PI / 7000.0f);
        float breathe = 0.35f + 0.25f * (0.5f + 0.5f * sinf(t));
        uint8_t v = (uint8_t)(14.0f * breathe);
        rgb_apply_status(0, (uint8_t)(v / 2U), v);
        return;
    }

    float t = (float)(now_ms % 5000ULL) * (2.0f * (float)M_PI / 5000.0f);
    float breathe = 0.45f + 0.55f * (0.5f + 0.5f * sinf(t));

    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    switch (s_screen) {
    case DASHCDG_HW_SCREEN_HOME: {
        wifi_ap_record_t ap;
        bool assoc = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
        if (assoc) {
            /* Soft heartbeat: cool green/cyan when associated. */
            r = (uint8_t)(15 * breathe);
            g = (uint8_t)(160 * breathe);
            b = (uint8_t)(110 * breathe);
        } else {
            /* Idle / no AP: warm amber "breathing". */
            r = (uint8_t)(200 * breathe);
            g = (uint8_t)(90 * breathe);
            b = (uint8_t)(18 * breathe);
        }
        break;
    }
    case DASHCDG_HW_SCREEN_WIFI:
    case DASHCDG_HW_SCREEN_SETTINGS:
    case DASHCDG_HW_SCREEN_DISPLAY:
        r = (uint8_t)(80 * breathe);
        g = (uint8_t)(40 * breathe);
        b = (uint8_t)(140 * breathe);
        break;
    case DASHCDG_HW_SCREEN_KARAOKE:
        if (s_cdg_stream_ok) {
            r = (uint8_t)(10 * breathe);
            g = (uint8_t)(200 * breathe);
            b = (uint8_t)(140 * breathe);
        } else {
            r = (uint8_t)(160 * breathe);
            g = (uint8_t)(30 * breathe);
            b = (uint8_t)(120 * breathe);
        }
        break;
    default:
        break;
    }
    rgb_apply_status(r, g, b);
}

static void hw_task(void *arg)
{
    (void)arg;
    uint64_t last_bat_ms = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HW_TICK_MS));
        uint64_t now = dashcdg_clock_now_ms();

        if (now - last_bat_ms >= 400U) {
            last_bat_ms = now;
            sample_battery_update_cache();
        }
        if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(40)) == pdTRUE) {
            /* PENIRQ active low on touch (same pin as XPT2046 interrupt). */
            if (gpio_get_level(CYD_GPIO_TP_IRQ) == 0) {
                pm_bump_activity_locked(now);
            }

            pm_update_idle_locked(now);
            pm_backlight_fade_step_locked();

            bl_set_pct(s_bl_applied_pct);
            led_anim_frame(now);

            /* IO0: short press wakes from display sleep; long hold (~1.4s) starts sleep fade. */
            int lvl = gpio_get_level(DASHCDG_HW_GPIO_USER_BTN);
            if (lvl != 0) {
                s_btn_low_streak = 0;
                s_btn_block_until_hi = false;
            } else {
                if (s_btn_low_streak < 250) {
                    s_btn_low_streak++;
                }
                if (!s_btn_block_until_hi) {
                    if (s_pm_state == PM_SLEEP && s_btn_low_streak == USER_BTN_WAKE_FRAMES) {
                        ESP_LOGI(TAG, "user button wake (IO0)");
                        pm_bump_activity_locked(now);
                        s_beep_until_ms = now + (uint64_t)BEEP_MS;
                    } else if (s_pm_state != PM_SLEEP && s_pm_state != PM_WAKE_FADE && s_pm_state != PM_SLEEP_FADE &&
                               s_btn_low_streak == USER_BTN_SLEEP_FRAMES) {
                        ESP_LOGI(TAG, "user button sleep (IO0 hold)");
                        pm_request_manual_sleep_fade_locked();
                        s_btn_block_until_hi = true;
                        s_beep_until_ms = now + (uint64_t)BEEP_MS;
                    }
                }
            }

            if (s_beep_until_ms != 0U) {
                if (now < s_beep_until_ms) {
                    beep_pwm_on();
                } else {
                    s_beep_until_ms = 0U;
                    beep_pwm_off();
                }
            } else {
                beep_pwm_off();
            }
            xSemaphoreGive(s_mtx);
        }
    }
}

esp_err_t dashcdg_platform_hw_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) {
        return ESP_ERR_NO_MEM;
    }

    if (!dashcdg_vbat_sense_is_ready()) {
        esp_err_t v = dashcdg_vbat_sense_init();
        if (v != ESP_OK) {
            ESP_LOGW(TAG, "vbat init in platform_hw: %s", esp_err_to_name(v));
        }
    }

    gpio_reset_pin(DASHCDG_HW_GPIO_LCD_BL_PWM);
    gpio_reset_pin(DASHCDG_HW_GPIO_RGB_R);
    gpio_reset_pin(DASHCDG_HW_GPIO_RGB_G);
    gpio_reset_pin(DASHCDG_HW_GPIO_RGB_B);
    gpio_reset_pin(DASHCDG_HW_GPIO_AUDIO_PWM);

    ESP_RETURN_ON_ERROR(gpio_misc_init(), TAG, "gpio_misc");
    ESP_RETURN_ON_ERROR(ledc_install_rgb_bl(), TAG, "ledc rgb/bl");
    ESP_RETURN_ON_ERROR(ledc_install_beep(), TAG, "ledc beep");
    (void)i2c_bus_init();

    {
        uint8_t bl = 100;
        (void)dashcdg_badge_prefs_load_brightness(&bl);
        s_bl_user_pct = bl;
        s_bl_applied_pct = bl;
    }
    {
        uint8_t r_on = 1;
        uint8_t r_pct = 100;
        (void)dashcdg_badge_prefs_load_rgb_status(&r_on, &r_pct);
        s_rgb_status_enabled = (r_on != 0);
        s_rgb_status_pct = r_pct;
    }
    {
        uint8_t a_on = 1;
        (void)dashcdg_badge_prefs_load_auto_sleep(&a_on);
        s_auto_sleep_enabled = (a_on != 0);
    }
    {
        uint8_t bv = 85;
        (void)dashcdg_badge_prefs_load_beep_volume(&bv);
        if (bv < 5U) {
            bv = 5U;
        }
        if (bv > 100U) {
            bv = 100U;
        }
        s_beep_vol_pct = bv;
    }
    {
        uint8_t tb = 1;
        (void)dashcdg_badge_prefs_load_touch_beep(&tb);
        s_touch_beep_on = (tb != 0);
    }
    bl_set_pct(s_bl_applied_pct);
    s_activity_ms = dashcdg_clock_now_ms();
    s_pm_state = PM_ACTIVE;
    __atomic_store_n(&s_disp_pwrcmd, 0, __ATOMIC_SEQ_CST);
    s_screen = DASHCDG_HW_SCREEN_HOME;
    s_cdg_stream_ok = false;
    sample_battery_update_cache();

    BaseType_t ok = xTaskCreate(hw_task, "dashcdg_hw", HW_TASK_STACK, NULL, HW_TASK_PRIO, &s_hw_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }
    s_ready = true;
    ESP_LOGI(TAG, "platform hw task started (prio %d)", HW_TASK_PRIO);
    return ESP_OK;
}

bool dashcdg_platform_hw_is_ready(void)
{
    return s_ready;
}

void dashcdg_platform_hw_backlight_set_pct(uint8_t pct_0_100)
{
    if (pct_0_100 > 100U) {
        pct_0_100 = 100U;
    }
    if (pct_0_100 < 5U) {
        pct_0_100 = 5U;
    }
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_bl_user_pct = pct_0_100;
        if (s_pm_state == PM_ACTIVE) {
            s_bl_applied_pct = pct_0_100;
        }
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_set_screen(dashcdg_hw_screen_t s)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_screen = s;
        pm_bump_activity_locked(dashcdg_clock_now_ms());
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_set_cdg_stream_ok(bool ok)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_cdg_stream_ok = ok;
        if (ok && s_screen == DASHCDG_HW_SCREEN_KARAOKE) {
            pm_bump_activity_locked(dashcdg_clock_now_ms());
        }
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_touch_click(void)
{
    if (!s_ready || !s_mtx) {
        return;
    }
    uint64_t now = dashcdg_clock_now_ms();
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(10)) == pdTRUE) {
        pm_bump_activity_locked(now);
        if (s_touch_beep_on) {
            s_beep_until_ms = now + (uint64_t)BEEP_MS;
        }
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_notify_activity(void)
{
    if (!s_ready || !s_mtx) {
        return;
    }
    uint64_t now = dashcdg_clock_now_ms();
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(25)) == pdTRUE) {
        pm_bump_activity_locked(now);
        xSemaphoreGive(s_mtx);
    }
}

int dashcdg_platform_hw_peek_display_power_cmd(void)
{
    return (int)__atomic_load_n(&s_disp_pwrcmd, __ATOMIC_SEQ_CST);
}

void dashcdg_platform_hw_ack_display_power_cmd(void)
{
    __atomic_store_n(&s_disp_pwrcmd, 0, __ATOMIC_SEQ_CST);
}

void dashcdg_platform_hw_set_rgb_status_enabled(bool on)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_rgb_status_enabled = on;
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_set_rgb_status_brightness(uint8_t pct_5_100)
{
    if (pct_5_100 < 5U) {
        pct_5_100 = 5U;
    }
    if (pct_5_100 > 100U) {
        pct_5_100 = 100U;
    }
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_rgb_status_pct = pct_5_100;
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_set_auto_sleep_enabled(bool on)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_auto_sleep_enabled = on;
        if (!on) {
            pm_force_active_restore_locked();
        }
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_set_touch_beep_enabled(bool on)
{
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_touch_beep_on = on;
        xSemaphoreGive(s_mtx);
    }
}

void dashcdg_platform_hw_set_beep_volume_pct(uint8_t pct_5_100)
{
    if (pct_5_100 < 5U) {
        pct_5_100 = 5U;
    }
    if (pct_5_100 > 100U) {
        pct_5_100 = 100U;
    }
    if (!s_mtx) {
        return;
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_beep_vol_pct = pct_5_100;
        xSemaphoreGive(s_mtx);
    }
}

esp_err_t dashcdg_platform_hw_battery_read(int *out_raw, int *out_pin_mv, int *out_vbat_mv)
{
    if (!s_ready || !s_mtx) {
        return dashcdg_vbat_sense_read(out_raw, out_pin_mv, out_vbat_mv);
    }
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(80)) != pdTRUE) {
        return dashcdg_vbat_sense_read(out_raw, out_pin_mv, out_vbat_mv);
    }
    if (out_raw) {
        *out_raw = s_bat_raw;
    }
    if (out_pin_mv) {
        *out_pin_mv = s_bat_pin_mv;
    }
    if (out_vbat_mv) {
        *out_vbat_mv = s_bat_vbat_mv;
    }
    (void)s_bat_sample_ms;
    xSemaphoreGive(s_mtx);
    return ESP_OK;
}
