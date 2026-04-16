#include "dashcdg/amr_codec.h"

#include <stdlib.h>
#include <string.h>

#include "typedef.h"
#include "enc_if.h"
#include "dec_if.h"

#define DASHCDG_AMR_WB_PCM16K 320
#define DASHCDG_AMR_WB_PCM48K 960
#define DASHCDG_AMR_WB_MODE 8

static void dashcdg_downsample_48k_to_16k_mono(const int16_t *in48, int16_t *out16) {
    size_t i;
    for (i = 0; i < DASHCDG_AMR_WB_PCM16K; ++i) {
        int32_t s = (int32_t) in48[i * 3U] + (int32_t) in48[i * 3U + 1U] + (int32_t) in48[i * 3U + 2U];
        out16[i] = (int16_t) (s / 3);
    }
}

static void dashcdg_upsample_16k_to_48k_mono(const int16_t *in16, int16_t *out48) {
    size_t i;
    for (i = 0; i < DASHCDG_AMR_WB_PCM16K; ++i) {
        int16_t s = in16[i];
        out48[i * 3U] = s;
        out48[i * 3U + 1U] = s;
        out48[i * 3U + 2U] = s;
    }
}

void dashcdg_amr_wb_encoder_create(void **opaque) {
    if (opaque == NULL) {
        return;
    }
    *opaque = E_IF_init();
}

void dashcdg_amr_wb_encoder_destroy(void *opaque) {
    if (opaque != NULL) {
        E_IF_exit(opaque);
    }
}

int dashcdg_amr_wb_encoder_run(void *opaque, const int16_t *pcm48_960, uint8_t *out, size_t out_cap) {
    Word16 pcm16k[DASHCDG_AMR_WB_PCM16K];
    int n;

    if (opaque == NULL || pcm48_960 == NULL || out == NULL || out_cap < (size_t) NB_SERIAL_MAX) {
        return 0;
    }
    dashcdg_downsample_48k_to_16k_mono(pcm48_960, pcm16k);
    n = E_IF_encode(opaque, (Word16) DASHCDG_AMR_WB_MODE, pcm16k, out, 0);
    if (n <= 0 || (size_t) n > out_cap) {
        return 0;
    }
    return n;
}

void dashcdg_amr_wb_decoder_create(void **opaque) {
    if (opaque == NULL) {
        return;
    }
    *opaque = D_IF_init();
}

void dashcdg_amr_wb_decoder_destroy(void *opaque) {
    if (opaque != NULL) {
        D_IF_exit(opaque);
    }
}

int dashcdg_amr_wb_decoder_run(void *opaque, const uint8_t *in, size_t in_len, int16_t *pcm48_960, size_t pcm_cap_samples) {
    Word16 pcm16k[DASHCDG_AMR_WB_PCM16K];

    if (opaque == NULL || in == NULL || in_len == 0U || pcm48_960 == NULL || pcm_cap_samples < DASHCDG_AMR_WB_PCM48K) {
        return 0;
    }
    if (in_len > (size_t) NB_SERIAL_MAX) {
        return 0;
    }
    {
        UWord8 bits[NB_SERIAL_MAX];
        memcpy(bits, in, in_len);
        D_IF_decode(opaque, bits, pcm16k, _good_frame);
    }
    dashcdg_upsample_16k_to_48k_mono(pcm16k, pcm48_960);
    return (int) DASHCDG_AMR_WB_PCM48K;
}
