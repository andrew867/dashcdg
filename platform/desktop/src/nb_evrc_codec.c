#include "dashcdg/nb_codec_adapters.h"

#include <stdlib.h>
#include <string.h>

#include "evrcc.h"

#define EVRC_FRAME8K 160
#define PCM48_FRAME 960

static void dashcdg_pcm48_stereo_to_mono8k_avg6(
        const int16_t *pcm48,
        size_t stereo_samples,
        int16_t *mono8k,
        size_t mono_count
) {
    size_t i;

    for (i = 0; i < mono_count; ++i) {
        size_t base = i * 6U;
        int32_t acc = 0;
        size_t k;

        if (base + 5U >= stereo_samples / 2U) {
            break;
        }
        for (k = 0; k < 6U; ++k) {
            size_t ix = (base + k) * 2U;

            acc += (int32_t) pcm48[ix] + (int32_t) pcm48[ix + 1U];
        }
        mono8k[i] = (int16_t) (acc / 12);
    }
    for (; i < mono_count; ++i) {
        mono8k[i] = 0;
    }
}

static void dashcdg_mono8k_hold6_to_pcm48_stereo(const int16_t *mono8k, int16_t *pcm48, size_t stereo_samples) {
    size_t i;
    size_t out_pairs = stereo_samples / 2U;

    for (i = 0; i < out_pairs; ++i) {
        int16_t s = mono8k[i / 6U];
        size_t ix = i * 2U;

        pcm48[ix] = s;
        pcm48[ix + 1U] = s;
    }
}

int dashcdg_evrc_encoder_create(void **out_ctx) {
    void *enc;

    if (out_ctx == NULL) {
        return 0;
    }
    enc = evrc_encoder_init(4, 4, 0);
    if (enc == NULL) {
        return 0;
    }
    *out_ctx = enc;
    return 1;
}

void dashcdg_evrc_encoder_destroy(void *ctx) {
    if (ctx != NULL) {
        evrc_encoder_uninit(ctx);
    }
}

int dashcdg_evrc_encode_pcm48_stereo_frame(
        void *ctx,
        const int16_t *pcm48_interleaved,
        size_t pcm_samples,
        uint8_t *out,
        size_t out_max
) {
    int16_t mono8k[EVRC_FRAME8K];
    int n;

    if (ctx == NULL || pcm48_interleaved == NULL || out == NULL || out_max == 0U) {
        return -1;
    }
    if (pcm_samples < PCM48_FRAME * 2U) {
        return -1;
    }
    dashcdg_pcm48_stereo_to_mono8k_avg6(pcm48_interleaved, pcm_samples, mono8k, EVRC_FRAME8K);
    n = evrc_encoder_encode_to_packet(ctx, mono8k, EVRC_FRAME8K, out, out_max);
    if (n <= 0) {
        return -1;
    }
    return n;
}

int dashcdg_evrc_decoder_create(void **out_ctx) {
    void *dec;

    if (out_ctx == NULL) {
        return 0;
    }
    dec = evrc_decoder_init();
    if (dec == NULL) {
        return 0;
    }
    *out_ctx = dec;
    return 1;
}

void dashcdg_evrc_decoder_destroy(void *ctx) {
    if (ctx != NULL) {
        evrc_decoder_uninit(ctx);
    }
}

int dashcdg_evrc_decode_to_pcm48_stereo(
        void *ctx,
        const uint8_t *in,
        size_t in_len,
        int16_t *pcm48_interleaved,
        size_t pcm_samples_max
) {
    int16_t mono8k[EVRC_FRAME8K];
    int got;

    if (ctx == NULL || in == NULL || in_len == 0U || pcm48_interleaved == NULL) {
        return -1;
    }
    if (pcm_samples_max < PCM48_FRAME * 2U) {
        return -1;
    }
    got = evrc_decoder_decode_from_packet(ctx, in, in_len, mono8k, EVRC_FRAME8K);
    if (got < (int) (EVRC_FRAME8K * (int) sizeof(int16_t))) {
        return -1;
    }
    dashcdg_mono8k_hold6_to_pcm48_stereo(mono8k, pcm48_interleaved, PCM48_FRAME * 2U);
    return (int) PCM48_FRAME;
}
