#pragma once

#include <stdint.h>

#include "dashcdg/cdg.h"

/** Visible CDG canvas (288x192) to packed RGB565 LE for LVGL (full visible window). */
void dashcdg_badge_cdg_state_to_rgb565_le(const struct dashcdg_cdg_state *state, uint16_t *dst);

/**
 * Same conversion for a sub-rectangle of the visible window (vx,vy relative to 0..288 x 0..192).
 * Pixels are written row-major into dst with stride dst_stride_px (>= vw). Plan C: band/scratch blits.
 */
void dashcdg_badge_cdg_state_rect_to_rgb565_le(const struct dashcdg_cdg_state *state, int vx0, int vy0, int vw, int vh,
                                             uint16_t *dst, size_t dst_stride_px);
