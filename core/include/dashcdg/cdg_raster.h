#ifndef DASHCDG_CDG_RASTER_H
#define DASHCDG_CDG_RASTER_H

#include "dashcdg/cdg.h"

/* 288 * 192 * 4 — MUST match docs/specs/cpu-rgba-raster-contract.md */
#define DASHCDG_CDG_RGBA_BYTES ((size_t) DASHCDG_VISIBLE_WIDTH * (size_t) DASHCDG_VISIBLE_HEIGHT * 4U)

void dashcdg_cdg_state_to_rgba8(const struct dashcdg_cdg_state *state, uint8_t *rgba_out);

#endif
