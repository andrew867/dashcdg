#include "sfx_touch.h"

#include <stdint.h>

#include "platform_hw.h"

static lv_obj_t *s_attached_screen;
static uint64_t s_last_touch_ms;

static void on_screen_touch(lv_event_t *e)
{
    (void)e;

    uint64_t now_ms = (uint64_t)lv_tick_get();
    if (s_last_touch_ms != 0U && now_ms > s_last_touch_ms && (now_ms - s_last_touch_ms) < 120U) {
        return;
    }
    s_last_touch_ms = now_ms;

    dashcdg_platform_hw_touch_click();
}

void dashcdg_sfx_touch_init(void)
{
}

void dashcdg_sfx_touch_attach_to_active_screen(lv_disp_t *disp)
{
    if (disp == NULL) {
        return;
    }
    lv_obj_t *scr = lv_disp_get_scr_act(disp);
    if (scr == NULL || scr == s_attached_screen) {
        return;
    }
    s_attached_screen = scr;
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, on_screen_touch, LV_EVENT_PRESSED, NULL);
}

