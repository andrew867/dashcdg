#include "dashcdg/media_clock.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

static int64_t dashcdg_clamp_delta(int64_t delta, int64_t max_step_ms) {
    if (max_step_ms < 0) {
        return delta;
    }

    if (delta > max_step_ms) {
        return max_step_ms;
    }

    if (delta < -max_step_ms) {
        return -max_step_ms;
    }

    return delta;
}

uint64_t dashcdg_clock_now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;

    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);

    return (uint64_t) ((counter.QuadPart * 1000ULL) / (uint64_t) frequency.QuadPart);
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ((uint64_t) ts.tv_sec * 1000ULL) + ((uint64_t) ts.tv_nsec / 1000000ULL);
#endif
}

void dashcdg_media_clock_init(struct dashcdg_media_clock *clock_state) {
    if (clock_state == NULL) {
        return;
    }

    clock_state->initialized = 0;
    clock_state->offset_ms = 0;
}

void dashcdg_media_clock_anchor(struct dashcdg_media_clock *clock_state, int64_t local_ms, int64_t remote_ms) {
    if (clock_state == NULL) {
        return;
    }

    clock_state->initialized = 1;
    clock_state->offset_ms = remote_ms - local_ms;
}

void dashcdg_media_clock_observe(
        struct dashcdg_media_clock *clock_state,
        int64_t local_ms,
        int64_t remote_ms,
        int64_t max_step_ms
) {
    int64_t target_offset;
    int64_t delta;

    if (clock_state == NULL) {
        return;
    }

    target_offset = remote_ms - local_ms;

    if (!clock_state->initialized) {
        clock_state->initialized = 1;
        clock_state->offset_ms = target_offset;
        return;
    }

    delta = target_offset - clock_state->offset_ms;
    clock_state->offset_ms += dashcdg_clamp_delta(delta, max_step_ms);
}

int64_t dashcdg_media_clock_remote_now(const struct dashcdg_media_clock *clock_state, int64_t local_ms) {
    if (clock_state == NULL || !clock_state->initialized) {
        return local_ms;
    }

    return local_ms + clock_state->offset_ms;
}
