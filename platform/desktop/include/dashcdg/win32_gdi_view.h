#ifndef DASHCDG_WIN32_GDI_VIEW_H
#define DASHCDG_WIN32_GDI_VIEW_H

#include <stddef.h>
#include <stdint.h>

struct dashcdg_win32_gdi_view;

typedef void (*dashcdg_win32_gdi_key_cb)(void *user, unsigned vk, int down);

int dashcdg_win32_gdi_view_create(
        struct dashcdg_win32_gdi_view **out,
        const char *title,
        int window_pixel_w,
        int window_pixel_h,
        dashcdg_win32_gdi_key_cb on_key,
        void *key_user
);

void dashcdg_win32_gdi_view_destroy(struct dashcdg_win32_gdi_view *view);

/*
 * Returns 1 while the window should stay open. When the user closes the window
 * or WM_QUIT is posted, returns 0.
 */
int dashcdg_win32_gdi_view_poll(struct dashcdg_win32_gdi_view *view);

/*
 * Blits the 288×192 RGBA buffer (from dashcdg_cdg_state_to_rgba8) into the
 * client area, then optional HUD lines in the top margin.
 */
int dashcdg_win32_gdi_view_present_rgba(
        struct dashcdg_win32_gdi_view *view,
        const uint8_t *rgba,
        size_t rgba_bytes,
        int show_hud,
        const char *hud_line_a,
        const char *hud_line_b,
        uint32_t hud_color_a_rgb,
        uint32_t hud_color_b_rgb
);

int dashcdg_win32_gdi_view_present_bgra(
        struct dashcdg_win32_gdi_view *view,
        const uint8_t *bgra,
        size_t bgra_bytes,
        int show_hud,
        const char *hud_line_a,
        const char *hud_line_b,
        uint32_t hud_color_a_rgb,
        uint32_t hud_color_b_rgb
);

#endif
