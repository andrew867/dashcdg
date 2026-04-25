#pragma once

#include <stdbool.h>

/** Audio lab: square-wave "Mary Had a Little Lamb" demo (~8 kHz) for PWM PCM; volume follows speaker slider. */
void dashcdg_badge_lab_ym_reset(void);
void dashcdg_badge_lab_ym_init(void);
void dashcdg_badge_lab_ym_play_set(bool play);
void dashcdg_badge_lab_ym_stop(void);
