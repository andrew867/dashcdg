#pragma once

#include <stdbool.h>

/** Tiny 3-osc “YM-ish” demo for Audio lab (square waves + arp); output is mono samples for PWM PCM. */
void dashcdg_badge_lab_ym_reset(void);
void dashcdg_badge_lab_ym_init(void);
void dashcdg_badge_lab_ym_play_set(bool play);
void dashcdg_badge_lab_ym_stop(void);
