#ifndef DASHCDG_COMMON_H
#define DASHCDG_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define DASHCDG_SCREEN_WIDTH 300
#define DASHCDG_SCREEN_HEIGHT 216
#define DASHCDG_VISIBLE_X 6
#define DASHCDG_VISIBLE_Y 12
#define DASHCDG_VISIBLE_RIGHT 294
#define DASHCDG_VISIBLE_BOTTOM 204
#define DASHCDG_VISIBLE_WIDTH (DASHCDG_VISIBLE_RIGHT - DASHCDG_VISIBLE_X)
#define DASHCDG_VISIBLE_HEIGHT (DASHCDG_VISIBLE_BOTTOM - DASHCDG_VISIBLE_Y)
#define DASHCDG_TILE_WIDTH 6
#define DASHCDG_TILE_HEIGHT 12
#define DASHCDG_COLORS 16
#define DASHCDG_PACKETS_PER_SECOND 300

#define DASHCDG_ARRAY_INDEX(x, y) (((y) * DASHCDG_SCREEN_WIDTH) + (x))

typedef uint64_t dashcdg_tick_t;

static inline uint64_t dashcdg_ms_to_packet_count(uint64_t ms) {
    return (ms * DASHCDG_PACKETS_PER_SECOND + 500ULL) / 1000ULL;
}

static inline uint64_t dashcdg_packet_count_to_ms(uint64_t packets) {
    /*
     * Integer division truncates subchannel time toward zero, which biases CDG
     * batches slightly early versus the 20 ms audio grid. Rounding keeps the
     * CDG timeline aligned with MP3-backed audio across long songs.
     */
    return (packets * 1000ULL + DASHCDG_PACKETS_PER_SECOND / 2ULL) / DASHCDG_PACKETS_PER_SECOND;
}

#endif
