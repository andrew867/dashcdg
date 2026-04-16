#ifndef DASHCDG_NB_IMA_CODEC_H
#define DASHCDG_NB_IMA_CODEC_H

#include <stddef.h>
#include <stdint.h>

/*
 * DashCDG narrowband audio — IMA ADPCM–style nibble stream on an 8 kHz-equivalent
 * timeline, carried inside 20 ms @ 48 kHz mono PCM frames on the desktop path.
 *
 * Implementation constraints (embedded / ESP32 portability):
 * - No floating-point types or libm calls in this translation unit.
 * - Arithmetic uses int16_t / int32_t only in the hot path.
 * - No heap allocation.
 *
 * Wire: v4 narrowband family (see dashcdg_v4_audio_codec_is_narrowband) uses
 * this payload for ids 2–7 today; id 2 (`sbc-like`) is the canonical name for
 * the same bytes as the historical “SBC-like” label (not Bluetooth SBC).
 */

#define DASHCDG_NB_IMA_CORE_SAMPLE_RATE_HZ 8000U
#define DASHCDG_NB_IMA_FRAME_SAMPLES 160U
#define DASHCDG_NB_IMA_PCM48_INPUT_SAMPLES 960U
#define DASHCDG_NB_IMA_ENCODED_BYTES (4U + (DASHCDG_NB_IMA_FRAME_SAMPLES / 2U))

struct dashcdg_nb_ima_state {
    int16_t predictor;
    uint8_t step_index;
};

void dashcdg_nb_ima_state_init(struct dashcdg_nb_ima_state *state);

int dashcdg_nb_ima_encode_pcm48_mono_frame(
        struct dashcdg_nb_ima_state *state,
        const int16_t *pcm_48k_mono,
        size_t pcm_samples,
        uint8_t *encoded,
        size_t encoded_capacity
);

int dashcdg_nb_ima_decode_to_pcm48_mono_frame(
        struct dashcdg_nb_ima_state *state,
        const uint8_t *encoded,
        size_t encoded_length,
        int16_t *pcm_48k_mono,
        size_t pcm_capacity_samples
);

#endif
