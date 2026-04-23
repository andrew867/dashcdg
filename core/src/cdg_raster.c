#include "dashcdg/cdg_raster.h"

#include <string.h>

static uint8_t dashcdg_alpha_from_transparency(uint8_t t) {
    return (uint8_t) (((uint16_t) (63U - (uint16_t) t) * 255U) / 63U);
}

void dashcdg_cdg_state_to_rgba8(const struct dashcdg_cdg_state *state, uint8_t *rgba_out) {
    int hx;
    int vy;
    uint8_t palette[16][4];

    if (state == NULL || rgba_out == NULL) {
        return;
    }

    hx = state->display_h_offset >= DASHCDG_TILE_WIDTH ? DASHCDG_TILE_WIDTH - 1 : (int) state->display_h_offset;
    vy = state->display_v_offset >= DASHCDG_TILE_HEIGHT ? DASHCDG_TILE_HEIGHT - 1 : (int) state->display_v_offset;
    for (size_t i = 0; i < 16U; ++i) {
        int rgb_packed = state->color_table[i] & 0xFFFFFF;
        uint8_t a = dashcdg_alpha_from_transparency(state->transparency[i]);

        palette[i][0] = (uint8_t) ((rgb_packed >> 16) & 0xFF);
        palette[i][1] = (uint8_t) ((rgb_packed >> 8) & 0xFF);
        palette[i][2] = (uint8_t) (rgb_packed & 0xFF);
        palette[i][3] = a;
    }

    for (int y = 0; y < DASHCDG_VISIBLE_HEIGHT; ++y) {
        for (int x = 0; x < DASHCDG_VISIBLE_WIDTH; ++x) {
            int sx = x + DASHCDG_VISIBLE_X + hx;
            int sy = y + DASHCDG_VISIBLE_Y + vy;

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
                uint8_t *pixel = rgba_out + (((size_t) y * (size_t) DASHCDG_VISIBLE_WIDTH + (size_t) x) * 4U);
                const uint8_t *color = palette[(size_t) c & 0x0F];

                pixel[0] = color[0];
                pixel[1] = color[1];
                pixel[2] = color[2];
                pixel[3] = color[3];
            }
        }
    }
}

void dashcdg_cdg_state_to_bgra8(const struct dashcdg_cdg_state *state, uint8_t *bgra_out) {
    int hx;
    int vy;
    uint8_t palette[16][4];

    if (state == NULL || bgra_out == NULL) {
        return;
    }

    hx = state->display_h_offset >= DASHCDG_TILE_WIDTH ? DASHCDG_TILE_WIDTH - 1 : (int) state->display_h_offset;
    vy = state->display_v_offset >= DASHCDG_TILE_HEIGHT ? DASHCDG_TILE_HEIGHT - 1 : (int) state->display_v_offset;
    for (size_t i = 0; i < 16U; ++i) {
        int rgb_packed = state->color_table[i] & 0xFFFFFF;
        uint8_t a = dashcdg_alpha_from_transparency(state->transparency[i]);

        palette[i][0] = (uint8_t) (rgb_packed & 0xFF);
        palette[i][1] = (uint8_t) ((rgb_packed >> 8) & 0xFF);
        palette[i][2] = (uint8_t) ((rgb_packed >> 16) & 0xFF);
        palette[i][3] = a;
    }

    for (int y = 0; y < DASHCDG_VISIBLE_HEIGHT; ++y) {
        for (int x = 0; x < DASHCDG_VISIBLE_WIDTH; ++x) {
            int sx = x + DASHCDG_VISIBLE_X + hx;
            int sy = y + DASHCDG_VISIBLE_Y + vy;

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
                uint8_t *pixel = bgra_out + (((size_t) y * (size_t) DASHCDG_VISIBLE_WIDTH + (size_t) x) * 4U);
                const uint8_t *color = palette[(size_t) c & 0x0F];

                pixel[0] = color[0];
                pixel[1] = color[1];
                pixel[2] = color[2];
                pixel[3] = color[3];
            }
        }
    }
}
