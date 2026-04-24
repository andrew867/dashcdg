/*
 * DashCDG badge / daughterboard GPIO map (MHP5050RGBDT + SC8002B + external I2C + PWM audio feed).
 * CYD LCD/touch pins stay in board_cyd_freenove_32.h (IO27 BL is handed to LEDC in platform_hw).
 *
 * RGB: active-low drive (GPIO low = LED on). 1k to 3V3 per your netlist - LEDC duty is inverted.
 * Backlight IO27: active-high (MOSFET gate, 10k pulldown on board).
 * Amp shutdown (SC8002B): 10k to 3V3 on /SHDN. Datasheet: VDD on shutdown pin => shutdown (IQ ~0.6uA).
 *   So HIGH = shutdown (mute), LOW = amplifier active.
 * IO0: 10k to 3V3, button to GND when pressed (active low). Strapping: avoid holding low across chip reset.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c.h"

/* MHP5050RGBDT - red IO16, green IO22, blue IO17 (active-low cathodes / common anode to 3V3). */
#define DASHCDG_HW_GPIO_RGB_R GPIO_NUM_16
#define DASHCDG_HW_GPIO_RGB_G GPIO_NUM_22
#define DASHCDG_HW_GPIO_RGB_B GPIO_NUM_17

#define DASHCDG_HW_GPIO_LCD_BL_PWM GPIO_NUM_27

#define DASHCDG_HW_GPIO_USER_BTN GPIO_NUM_0

/** SC8002B shutdown (10k to 3V3): HIGH = shutdown/mute, LOW = amp running. */
#define DASHCDG_HW_GPIO_AMP_SHUTDOWN GPIO_NUM_4

/**
 * Class-D feed from ESP32 PWM (LEDC on IO26). Analog network:
 *   IO26 -> 4k7 -> (10n + 100p shunt to GND) -> 0.39u DC block -> 20k -> IN- (SC8002B),
 *   20k from IN- to SP+ (per your schematic). Tune shunt / series for HF roll-off vs. click.
 *   Datasheet THD+N is quoted at AVD=2 into 8R; divider + default ~50% PWM duty made taps quiet - use
 *   Display settings "beep level" (NVS) to raise LEDC duty toward full swing.
 */
#define DASHCDG_HW_GPIO_AUDIO_PWM GPIO_NUM_26

/** External I2C bus (no devices registered yet; bus is parked ready). */
#define DASHCDG_HW_GPIO_I2C_SDA GPIO_NUM_32
#define DASHCDG_HW_GPIO_I2C_SCL GPIO_NUM_25

#define DASHCDG_HW_I2C_PORT       I2C_NUM_0
#define DASHCDG_HW_I2C_FREQ_HZ  100000

/*
 * Auto idle: home uses touch/nav activity only. Karaoke stays at user brightness while
 * `dashcdg_platform_hw_set_cdg_stream_ok(true)` or multicast UDP arrived within the last
 * `DASHCDG_HW_IDLE_DIM_MS` (see `dashcdg_platform_hw_note_karaoke_mcast_rx` and
 * `dashcdg_platform_hw_note_karaoke_cdg_overlay_tick`); then the same
 * dim/sleep timers apply. IO0 long-hold always forces sleep from any screen.
 * Override from `-D` or a wrapper header before including this file if needed.
 */
#ifndef DASHCDG_HW_IDLE_DIM_MS
/** No user activity: first phase - ramp LCD backlight down (ms). */
#define DASHCDG_HW_IDLE_DIM_MS 30000U
#endif
#ifndef DASHCDG_HW_IDLE_SLEEP_MS
/** Second phase - backlight off, ST7789 sleep, Wi-Fi max PS (ms, from last activity). */
#define DASHCDG_HW_IDLE_SLEEP_MS 60000U
#endif
#ifndef DASHCDG_HW_IDLE_DIM_PCT_OF_MAX
/** Dimmed target = (user max backlight %) * this / 100 (e.g. 22 ~ 22% of the saved slider cap). */
#define DASHCDG_HW_IDLE_DIM_PCT_OF_MAX 22U
#endif
#ifndef DASHCDG_HW_IDLE_DIM_MIN_PCT
/** Floor while dimmed so the panel is not fully black before sleep (1-100). */
#define DASHCDG_HW_IDLE_DIM_MIN_PCT 6U
#endif
#ifndef DASHCDG_HW_IDLE_DIM_RAMP_DIV
/** Larger = slower ramp toward dim target each platform tick (integer >= 1). */
#define DASHCDG_HW_IDLE_DIM_RAMP_DIV 6U
#endif

/* Example soak (edit above or pass -D from CMake):
 *   #define DASHCDG_HW_IDLE_DIM_MS     45000U
 *   #define DASHCDG_HW_IDLE_SLEEP_MS   90000U
 *   #define DASHCDG_HW_IDLE_DIM_PCT_OF_MAX 20U
 * Keep DASHCDG_HW_IDLE_DIM_MS < DASHCDG_HW_IDLE_SLEEP_MS so the dim phase is visible. */
