#pragma once

#include "lvgl.h"

/* Touch-anywhere feedback, routed through platform_hw so DAC owners and prefs are respected. */
void dashcdg_sfx_touch_init(void);
void dashcdg_sfx_touch_attach_to_active_screen(lv_disp_t *disp);

