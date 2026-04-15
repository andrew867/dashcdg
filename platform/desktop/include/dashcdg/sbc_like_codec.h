#ifndef DASHCDG_SBC_LIKE_CODEC_H
#define DASHCDG_SBC_LIKE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#define DASHCDG_SBC_LIKE_SAMPLE_RATE 8000U
#define DASHCDG_SBC_LIKE_FRAME_SAMPLES 160U
#define DASHCDG_SBC_LIKE_INPUT_SAMPLES 960U
#define DASHCDG_SBC_LIKE_ENCODED_BYTES (4U + (DASHCDG_SBC_LIKE_FRAME_SAMPLES / 2U))

struct dashcdg_sbc_like_encoder {
    int16_t predictor;
    uint8_t step_index;
};

struct dashcdg_sbc_like_decoder {
    int16_t predictor;
    uint8_t step_index;
};

void dashcdg_sbc_like_encoder_init(struct dashcdg_sbc_like_encoder *encoder);
int dashcdg_sbc_like_encode_frame(
        struct dashcdg_sbc_like_encoder *encoder,
        const int16_t *pcm_48k,
        size_t pcm_samples,
        uint8_t *encoded,
        size_t encoded_capacity
);

void dashcdg_sbc_like_decoder_init(struct dashcdg_sbc_like_decoder *decoder);
int dashcdg_sbc_like_decode_frame(
        struct dashcdg_sbc_like_decoder *decoder,
        const uint8_t *encoded,
        size_t encoded_length,
        int16_t *pcm_48k,
        size_t pcm_capacity
);

#endif
