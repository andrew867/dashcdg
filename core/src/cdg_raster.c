#include "dashcdg/cdg_raster.h"

#include <string.h>

static uint8_t dashcdg_alpha_from_transparency(uint8_t t) {
    return (uint8_t) (((uint16_t) (63U - (uint16_t) t) * 255U) / 63U);
}

void dashcdg_cdg_state_to_rgba8(const struct dashcdg_cdg_state *state, uint8_t *rgba_out) {
    int hx;
    int vy;

    if (state == NULL || rgba_out == NULL) {
        return;
    }

    hx = state->display_h_offset >= DASHCDG_TILE_WIDTH ? DASHCDG_TILE_WIDTH - 1 : (int) state->display_h_offset;
    vy = state->display_v_offset >= DASHCDG_TILE_HEIGHT ? DASHCDG_TILE_HEIGHT - 1 : (int) state->display_v_offset;

    for (int y = 0; y < DASHCDG_VISIBLE_HEIGHT; ++y) {
        for (int x = 0; x < DASHCDG_VISIBLE_WIDTH; ++x) {
            int sx = x + DASHCDG_VISIBLE_X + hx;
            int sy = y + DASHCDG_VISIBLE_Y + vy;
            size_t o = ((size_t) y * (size_t) DASHCDG_VISIBLE_WIDTH + (size_t) x) * 4U;

            if (sx < 0) {
                sx = 0;
            } else if (sx > DASHCDG_SCREEN_WIDTH - 1) {
                sx = DASHCDG_SCREEN_WIDTH - 1;
            }
            if (sy < 0) {
                sy = 0;
            } else if (sy > DASHCDG_SCREEN_HEIGHT - 1) {
                sy = DASHCDG_SCREEN_HEIGHT - 1;
            }

            {
                int idx = sy * DASHCDG_SCREEN_WIDTH + sx;
                uint8_t c = state->framebuffer[idx];
                int rgb_packed = state->color_table[(size_t) c & 0x0F] & 0xFFFFFF;
                uint8_t a = dashcdg_alpha_from_transparency(state->transparency[(size_t) c & 0x0F]);

                rgba_out[o + 0U] = (uint8_t) ((rgb_packed >> 16) & 0xFF);
                rgba_out[o + 1U] = (uint8_t) ((rgb_packed >> 8) & 0xFF);
                rgba_out[o + 2U] = (uint8_t) (rgb_packed & 0xFF);
                rgba_out[o + 3U] = a;
            }
        }
    }
}
