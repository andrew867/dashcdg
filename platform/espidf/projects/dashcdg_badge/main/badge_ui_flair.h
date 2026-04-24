#pragma once

/** One random line per call (uses esp_random); safe from LVGL / UI thread. */
const char *dashcdg_ui_flair_movie_tagline(void);
const char *dashcdg_ui_flair_wifi_sub(void);
const char *dashcdg_ui_flair_touch_cal_sub(void);
const char *dashcdg_ui_flair_display_sub(void);
const char *dashcdg_ui_flair_home_sub(void);
