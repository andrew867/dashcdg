#ifndef DASHCDG_NB_CODEC_ADAPTERS_H
#define DASHCDG_NB_CODEC_ADAPTERS_H

#include <stddef.h>
#include <stdint.h>

#define DASHCDG_QCELP13K_FRAME_BYTES (18U * sizeof(uint16_t))

int dashcdg_evrc_encoder_create(void **out_ctx);
void dashcdg_evrc_encoder_destroy(void *ctx);
int dashcdg_evrc_encode_pcm48_stereo_frame(
        void *ctx,
        const int16_t *pcm48_interleaved,
        size_t pcm_samples,
        uint8_t *out,
        size_t out_max
);

int dashcdg_evrc_decoder_create(void **out_ctx);
void dashcdg_evrc_decoder_destroy(void *ctx);
int dashcdg_evrc_decode_to_pcm48_stereo(
        void *ctx,
        const uint8_t *in,
        size_t in_len,
        int16_t *pcm48_interleaved,
        size_t pcm_samples_max
);

int dashcdg_qcelp13k_encoder_create(void **out_ctx);
void dashcdg_qcelp13k_encoder_destroy(void *ctx);
int dashcdg_qcelp13k_encode_pcm48_stereo_frame(
        void *ctx,
        const int16_t *pcm48_interleaved,
        size_t pcm_samples,
        uint8_t *out,
        size_t out_max
);

int dashcdg_qcelp13k_decoder_create(void **out_ctx);
void dashcdg_qcelp13k_decoder_destroy(void *ctx);
int dashcdg_qcelp13k_decode_to_pcm48_stereo(
        void *ctx,
        const uint8_t *in,
        size_t in_len,
        int16_t *pcm48_interleaved,
        size_t pcm_samples_max
);

int dashcdg_bt_sbc_encoder_create(void **out_ctx);
void dashcdg_bt_sbc_encoder_destroy(void *ctx);
int dashcdg_bt_sbc_encode_pcm48_stereo_frame(
        void *ctx,
        const int16_t *pcm48_interleaved,
        size_t pcm_samples,
        uint8_t *out,
        size_t out_max
);

int dashcdg_bt_sbc_decoder_create(void **out_ctx);
void dashcdg_bt_sbc_decoder_destroy(void *ctx);
int dashcdg_bt_sbc_decode_to_pcm48_stereo(
        void *ctx,
        const uint8_t *in,
        size_t in_len,
        int16_t *pcm48_interleaved,
        size_t pcm_samples_max
);

#endif
