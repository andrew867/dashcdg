/*
 * Minimal square-wave lab voice (~8 kHz tick). Not a full PSG emulator — enough to exercise IO26 PWM.
 */
#include "badge_lab_ym.h"

#include "platform_hw.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <string.h>

static const char *TAG = "badge_lab_ym";

#define LAB_TASK_STACK  3072
#define LAB_TASK_PRIO   5
#define LAB_SAMPLE_US   125U /* ~8000 Hz */

static TaskHandle_t s_task;
static volatile bool s_want_play;

static uint32_t s_phase[3];
static uint32_t s_inc[3];
static uint32_t s_tick;

static const uint16_t k_row0[] = {262, 294, 330, 349, 392, 440, 392, 349};
static const uint16_t k_row1[] = {196, 220, 196, 174, 196, 220, 247, 220};

static uint32_t hz_to_inc(uint16_t hz)
{
    return ((uint32_t)hz * (1U << 22)) / 8000U;
}

static void pattern_refresh(void)
{
    unsigned i0 = (unsigned)(s_tick / 512U) % (sizeof(k_row0) / sizeof(k_row0[0]));
    unsigned i1 = (unsigned)(s_tick / 384U) % (sizeof(k_row1) / sizeof(k_row1[0]));
    s_inc[0] = hz_to_inc(k_row0[i0]);
    s_inc[1] = hz_to_inc((uint16_t)(k_row1[i1] + 3U));
    s_inc[2] = hz_to_inc((uint16_t)(k_row0[i0] / 2U + 6U));
}

static int32_t ym_tick(void)
{
    s_tick++;
    if ((s_tick & 127U) == 0U) {
        pattern_refresh();
    }

    int32_t acc = 0;
    for (int i = 0; i < 3; i++) {
        s_phase[i] += s_inc[i];
        int32_t sq = (s_phase[i] & 0x80000000U) ? 2730 : -2730;
        acc += sq;
    }
    return acc / 3;
}

static uint8_t lab_sample_to_duty(int32_t sample)
{
    uint8_t pct = dashcdg_platform_hw_get_beep_volume_pct();
    float p = (float)pct;
    if (p < 5.f) {
        p = 5.f;
    }
    if (p > 100.f) {
        p = 100.f;
    }
    float t = (p - 5.f) / 95.f;
    float g = powf(t, 3.4f);
    uint32_t peak = (uint32_t)(g * 255.f + 0.5f);
    if (peak < 1U && pct >= 6U) {
        peak = 1U;
    }
    if (peak > 255U) {
        peak = 255U;
    }

    int32_t x = (sample * (int32_t)peak) / 4096;
    if (x > 100) {
        x = 100;
    }
    if (x < -100) {
        x = -100;
    }
    int32_t d = (int32_t)128 + x;
    if (d < 4) {
        d = 4;
    }
    if (d > 252) {
        d = 252;
    }
    return (uint8_t)d;
}

static void lab_task(void *arg)
{
    (void)arg;
    bool carrier_on = false;

    for (;;) {
        if (s_want_play) {
            if (!carrier_on) {
                dashcdg_badge_lab_ym_reset();
                dashcdg_platform_hw_lab_pcm_stream_begin();
                carrier_on = true;
            }
            int32_t s = ym_tick();
            uint8_t d = lab_sample_to_duty(s);
            dashcdg_platform_hw_lab_pcm_push_u8(d);
            esp_rom_delay_us(LAB_SAMPLE_US);
        } else {
            if (carrier_on) {
                dashcdg_platform_hw_lab_pcm_stream_end();
                carrier_on = false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

void dashcdg_badge_lab_ym_reset(void)
{
    memset(s_phase, 0, sizeof(s_phase));
    s_tick = 0U;
    pattern_refresh();
}

void dashcdg_badge_lab_ym_init(void)
{
    if (s_task != NULL) {
        return;
    }
    BaseType_t ok = xTaskCreate(lab_task, "dashcdg_lab_ym", LAB_TASK_STACK, NULL, LAB_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate lab_task failed");
        s_task = NULL;
    }
}

void dashcdg_badge_lab_ym_play_set(bool play)
{
    dashcdg_badge_lab_ym_init();
    s_want_play = play;
}

void dashcdg_badge_lab_ym_stop(void)
{
    s_want_play = false;
    dashcdg_platform_hw_lab_pcm_stream_end();
    if (s_task) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
