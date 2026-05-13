#pragma once

#include "lvgl.h"

/* Minimal proof-of-design: touch anywhere -> enqueue a short PCM chirp via audio_mgr. */
void dashcdg_sfx_touch_init(void);
void dashcdg_sfx_touch_attach_to_active_screen(lv_disp_t *disp);

