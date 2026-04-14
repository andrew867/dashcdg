#ifndef DASHCDG_MEDIA_CLOCK_H
#define DASHCDG_MEDIA_CLOCK_H

#include <stdint.h>

struct dashcdg_media_clock {
    int initialized;
    int64_t offset_ms;
};

uint64_t dashcdg_clock_now_ms(void);
void dashcdg_media_clock_init(struct dashcdg_media_clock *clock_state);
void dashcdg_media_clock_anchor(struct dashcdg_media_clock *clock_state, int64_t local_ms, int64_t remote_ms);
void dashcdg_media_clock_observe(
        struct dashcdg_media_clock *clock_state,
        int64_t local_ms,
        int64_t remote_ms,
        int64_t max_step_ms
);
int64_t dashcdg_media_clock_remote_now(const struct dashcdg_media_clock *clock_state, int64_t local_ms);

#endif
