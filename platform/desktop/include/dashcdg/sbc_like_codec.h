#ifndef DASHCDG_SBC_LIKE_CODEC_H
#define DASHCDG_SBC_LIKE_CODEC_H

/*
 * Compatibility aliases for the core narrowband codec (dashcdg/nb_ima_codec.h).
 * New code should include nb_ima_codec.h directly; "SBC-like" remains the user-
 * facing name for wire id 2 only (not Bluetooth A2DP SBC).
 */

#include "dashcdg/nb_ima_codec.h"

#define DASHCDG_SBC_LIKE_SAMPLE_RATE DASHCDG_NB_IMA_CORE_SAMPLE_RATE_HZ
#define DASHCDG_SBC_LIKE_FRAME_SAMPLES DASHCDG_NB_IMA_FRAME_SAMPLES
#define DASHCDG_SBC_LIKE_INPUT_SAMPLES DASHCDG_NB_IMA_PCM48_INPUT_SAMPLES
#define DASHCDG_SBC_LIKE_ENCODED_BYTES DASHCDG_NB_IMA_ENCODED_BYTES

typedef struct dashcdg_nb_ima_state dashcdg_sbc_like_encoder;
typedef struct dashcdg_nb_ima_state dashcdg_sbc_like_decoder;

static inline void dashcdg_sbc_like_encoder_init(struct dashcdg_sbc_like_encoder *encoder) {
    dashcdg_nb_ima_state_init((struct dashcdg_nb_ima_state *) encoder);
}

static inline int dashcdg_sbc_like_encode_frame(
        struct dashcdg_sbc_like_encoder *encoder,
        const int16_t *pcm_48k,
        size_t pcm_samples,
        uint8_t *encoded,
        size_t encoded_capacity
) {
    return dashcdg_nb_ima_encode_pcm48_mono_frame(
            (struct dashcdg_nb_ima_state *) encoder,
            pcm_48k,
            pcm_samples,
            encoded,
            encoded_capacity
    );
}

static inline void dashcdg_sbc_like_decoder_init(struct dashcdg_sbc_like_decoder *decoder) {
    dashcdg_nb_ima_state_init((struct dashcdg_nb_ima_state *) decoder);
}

static inline int dashcdg_sbc_like_decode_frame(
        struct dashcdg_sbc_like_decoder *decoder,
        const uint8_t *encoded,
        size_t encoded_length,
        int16_t *pcm_48k,
        size_t pcm_capacity
) {
    return dashcdg_nb_ima_decode_to_pcm48_mono_frame(
            (struct dashcdg_nb_ima_state *) decoder,
            encoded,
            encoded_length,
            pcm_48k,
            pcm_capacity
    );
}

#endif
