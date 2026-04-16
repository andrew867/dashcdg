#include "dashcdg/nb_ima_codec.h"

#include <string.h>

static const int g_dashcdg_ima_step_table[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
        19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
        130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
        5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int g_dashcdg_ima_index_table[16] = {
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8
};

static uint8_t dashcdg_ima_encode_nibble(int16_t sample, int16_t *predictor, uint8_t *step_index) {
    int predictor_work = *predictor;
    int step = g_dashcdg_ima_step_table[*step_index];
    int diff = sample - predictor_work;
    int delta = 0;
    int vpdiff = step >> 3;

    if (diff < 0) {
        delta = 8;
        diff = -diff;
    }
    if (diff >= step) {
        delta |= 4;
        diff -= step;
        vpdiff += step;
    }
    if (diff >= (step >> 1)) {
        delta |= 2;
        diff -= step >> 1;
        vpdiff += step >> 1;
    }
    if (diff >= (step >> 2)) {
        delta |= 1;
        vpdiff += step >> 2;
    }

    if ((delta & 8) != 0) {
        predictor_work -= vpdiff;
    } else {
        predictor_work += vpdiff;
    }

    if (predictor_work > 32767) {
        predictor_work = 32767;
    } else if (predictor_work < -32768) {
        predictor_work = -32768;
    }
    *predictor = (int16_t) predictor_work;

    {
        int next_index = (int) *step_index + g_dashcdg_ima_index_table[delta & 0x0F];
        if (next_index < 0) {
            next_index = 0;
        } else if (next_index > 88) {
            next_index = 88;
        }
        *step_index = (uint8_t) next_index;
    }

    return (uint8_t) (delta & 0x0F);
}

static int16_t dashcdg_ima_decode_nibble(uint8_t delta, int16_t *predictor, uint8_t *step_index) {
    int predictor_work = *predictor;
    int step = g_dashcdg_ima_step_table[*step_index];
    int vpdiff = step >> 3;

    if ((delta & 4U) != 0U) {
        vpdiff += step;
    }
    if ((delta & 2U) != 0U) {
        vpdiff += step >> 1;
    }
    if ((delta & 1U) != 0U) {
        vpdiff += step >> 2;
    }

    if ((delta & 8U) != 0U) {
        predictor_work -= vpdiff;
    } else {
        predictor_work += vpdiff;
    }

    if (predictor_work > 32767) {
        predictor_work = 32767;
    } else if (predictor_work < -32768) {
        predictor_work = -32768;
    }
    *predictor = (int16_t) predictor_work;

    {
        int next_index = (int) *step_index + g_dashcdg_ima_index_table[delta & 0x0F];
        if (next_index < 0) {
            next_index = 0;
        } else if (next_index > 88) {
            next_index = 88;
        }
        *step_index = (uint8_t) next_index;
    }

    return *predictor;
}

void dashcdg_nb_ima_state_init(struct dashcdg_nb_ima_state *state) {
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

int dashcdg_nb_ima_encode_pcm48_mono_frame(
        struct dashcdg_nb_ima_state *state,
        const int16_t *pcm_48k_mono,
        size_t pcm_samples,
        uint8_t *encoded,
        size_t encoded_capacity
) {
    int16_t downsampled[DASHCDG_NB_IMA_FRAME_SAMPLES];
    int16_t predictor;
    uint8_t step_index;

    if (state == NULL || pcm_48k_mono == NULL || encoded == NULL ||
            pcm_samples < DASHCDG_NB_IMA_PCM48_INPUT_SAMPLES ||
            encoded_capacity < DASHCDG_NB_IMA_ENCODED_BYTES) {
        return 0;
    }

    for (size_t i = 0; i < DASHCDG_NB_IMA_FRAME_SAMPLES; ++i) {
        int32_t sum = 0;
        for (size_t j = 0; j < 6U; ++j) {
            sum += pcm_48k_mono[(i * 6U) + j];
        }
        downsampled[i] = (int16_t) (sum / 6);
    }

    predictor = state->predictor;
    step_index = state->step_index;
    encoded[0] = (uint8_t) ((predictor >> 8) & 0xFF);
    encoded[1] = (uint8_t) (predictor & 0xFF);
    encoded[2] = step_index;
    encoded[3] = DASHCDG_NB_IMA_FRAME_SAMPLES;
    for (size_t i = 0; i < DASHCDG_NB_IMA_FRAME_SAMPLES; i += 2U) {
        uint8_t lo = dashcdg_ima_encode_nibble(downsampled[i], &predictor, &step_index);
        uint8_t hi = dashcdg_ima_encode_nibble(downsampled[i + 1U], &predictor, &step_index);
        encoded[4U + (i / 2U)] = (uint8_t) ((hi << 4U) | lo);
    }

    state->predictor = predictor;
    state->step_index = step_index;
    return (int) DASHCDG_NB_IMA_ENCODED_BYTES;
}

int dashcdg_nb_ima_decode_to_pcm48_mono_frame(
        struct dashcdg_nb_ima_state *state,
        const uint8_t *encoded,
        size_t encoded_length,
        int16_t *pcm_48k_mono,
        size_t pcm_capacity_samples
) {
    int16_t predictor;
    uint8_t step_index;
    int16_t decoded[DASHCDG_NB_IMA_FRAME_SAMPLES];
    uint8_t sample_count;

    if (state == NULL || encoded == NULL || pcm_48k_mono == NULL ||
            encoded_length < DASHCDG_NB_IMA_ENCODED_BYTES ||
            pcm_capacity_samples < DASHCDG_NB_IMA_PCM48_INPUT_SAMPLES) {
        return 0;
    }

    predictor = (int16_t) (((int16_t) encoded[0] << 8) | encoded[1]);
    step_index = encoded[2];
    sample_count = encoded[3];
    if (sample_count != DASHCDG_NB_IMA_FRAME_SAMPLES || step_index > 88U) {
        return 0;
    }

    for (size_t i = 0; i < DASHCDG_NB_IMA_FRAME_SAMPLES; i += 2U) {
        uint8_t packed = encoded[4U + (i / 2U)];
        decoded[i] = dashcdg_ima_decode_nibble((uint8_t) (packed & 0x0F), &predictor, &step_index);
        decoded[i + 1U] = dashcdg_ima_decode_nibble((uint8_t) ((packed >> 4U) & 0x0F), &predictor, &step_index);
    }

    for (size_t i = 0; i < DASHCDG_NB_IMA_FRAME_SAMPLES; ++i) {
        for (size_t j = 0; j < 6U; ++j) {
            pcm_48k_mono[(i * 6U) + j] = decoded[i];
        }
    }

    state->predictor = predictor;
    state->step_index = step_index;
    return (int) DASHCDG_NB_IMA_PCM48_INPUT_SAMPLES;
}
