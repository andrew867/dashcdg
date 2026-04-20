#ifndef DASHCDG_PCM_SOXR_STREAM_H
#define DASHCDG_PCM_SOXR_STREAM_H

#include <stddef.h>
#include <stdint.h>

/*
 * libsoxr (SoX Resampler, LGPL): streaming SRC for RX stereo int16.
 * Windows MinGW builds require -DDASHCDG_HAVE_LIBSOXR=1 and link libsoxr.a (see scripts/build_soxr_vendor.sh).
 * Non-Windows: optional when vendored lib exists.
 */

int dashcdg_pcm_soxr_stream_available(void);

void dashcdg_pcm_soxr_stream_reset(void);

/*
 * Prepare resampler for in_sr -> out_sr stereo interleaved. Call after asset/session reset
 * or when rates change. Returns 1 if the next process call may use soxr (built + valid rates).
 */
int dashcdg_pcm_soxr_stream_ensure(uint32_t in_sr, uint32_t out_sr);

/*
 * Resample one chunk. in_frames / out_frames are audio frames (not samples); stereo interleaved.
 * Returns 1 on success (fills out[]).
 */
int dashcdg_pcm_soxr_stream_process_interleaved(
        const int16_t *in,
        size_t in_frames,
        int16_t *out,
        size_t out_frames
);

#endif
