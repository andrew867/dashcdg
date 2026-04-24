#include "badge_cdg_rgb565.h"

#include <stddef.h>

#include "dashcdg/common.h"

static uint8_t dashcdg_alpha_from_transparency(uint8_t t)
{
    return (uint8_t)(((uint16_t)(63U - (uint16_t)t) * 255U) / 63U);
}

static uint16_t rgb888_to_rgb565_le(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    return v;
}

static void build_palette(const struct dashcdg_cdg_state *state, uint8_t palette[16][4])
{
    for (size_t i = 0; i < 16U; ++i) {
        int rgb_packed = state->color_table[i] & 0xFFFFFF;
        uint8_t a = dashcdg_alpha_from_transparency(state->transparency[i]);

        palette[i][0] = (uint8_t)((rgb_packed >> 16) & 0xFF);
        palette[i][1] = (uint8_t)((rgb_packed >> 8) & 0xFF);
        palette[i][2] = (uint8_t)(rgb_packed & 0xFF);
        palette[i][3] = a;
    }
}

void dashcdg_badge_cdg_state_rect_to_rgb565_le(const struct dashcdg_cdg_state *state, int vx0, int vy0, int vw, int vh,
                                             uint16_t *dst, size_t dst_stride_px)
{
    uint8_t palette[16][4];

    if (state == NULL || dst == NULL || vw <= 0 || vh <= 0) {
        return;
    }
    if (vx0 < 0) {
        vw += vx0;
        vx0 = 0;
    }
    if (vy0 < 0) {
        vh += vy0;
        vy0 = 0;
    }
    if (vx0 >= DASHCDG_VISIBLE_WIDTH || vy0 >= DASHCDG_VISIBLE_HEIGHT) {
        return;
    }
    if (vx0 + vw > DASHCDG_VISIBLE_WIDTH) {
        vw = DASHCDG_VISIBLE_WIDTH - vx0;
    }
    if (vy0 + vh > DASHCDG_VISIBLE_HEIGHT) {
        vh = DASHCDG_VISIBLE_HEIGHT - vy0;
    }
    if (vw <= 0 || vh <= 0) {
        return;
    }
    if (dst_stride_px < (size_t)vw) {
        return;
    }

    int hx = state->display_h_offset >= DASHCDG_TILE_WIDTH ? DASHCDG_TILE_WIDTH - 1 : (int)state->display_h_offset;
    int vy = state->display_v_offset >= DASHCDG_TILE_HEIGHT ? DASHCDG_TILE_HEIGHT - 1 : (int)state->display_v_offset;
    build_palette(state, palette);

    for (int row = 0; row < vh; ++row) {
        int y = vy0 + row;
        uint16_t *drow = dst + (size_t)row * dst_stride_px;
        for (int col = 0; col < vw; ++col) {
            int x = vx0 + col;
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

            int idx = sy * DASHCDG_SCREEN_WIDTH + sx;
            uint8_t c = state->framebuffer[idx];
            const uint8_t *color = palette[(size_t)c & 0x0F];
            drow[col] = rgb888_to_rgb565_le(color[0], color[1], color[2]);
        }
    }
}

void dashcdg_badge_cdg_state_to_rgb565_le(const struct dashcdg_cdg_state *state, uint16_t *dst)
{
    if (state == NULL || dst == NULL) {
        return;
    }
    dashcdg_badge_cdg_state_rect_to_rgb565_le(state, 0, 0, DASHCDG_VISIBLE_WIDTH, DASHCDG_VISIBLE_HEIGHT, dst,
                                            (size_t)DASHCDG_VISIBLE_WIDTH);
}
