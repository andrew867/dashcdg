#include "dashcdg/amr_codec.h"

#include <stdlib.h>
#include <string.h>

#include "interf_enc.h"
#include "interf_dec.h"
#include "sp_enc.h"

#define DASHCDG_AMR_NB_PCM8K 160
#define DASHCDG_AMR_NB_PCM48K 960

static void dashcdg_downsample_48k_to_8k_mono(const int16_t *in48, int16_t *out8) {
    size_t i;
    for (i = 0; i < DASHCDG_AMR_NB_PCM8K; ++i) {
        int32_t s = 0;
        size_t j;
        for (j = 0; j < 6U; ++j) {
            s += (int32_t) in48[i * 6U + j];
        }
        out8[i] = (int16_t) (s / 6);
    }
}

static void dashcdg_upsample_8k_to_48k_mono(const int16_t *in8, int16_t *out48) {
    size_t i;
    for (i = 0; i < DASHCDG_AMR_NB_PCM8K; ++i) {
        int16_t s = in8[i];
        size_t j;
        for (j = 0; j < 6U; ++j) {
            out48[i * 6U + j] = s;
        }
    }
}

void dashcdg_amr_nb_encoder_create(void **opaque) {
    if (opaque == NULL) {
        return;
    }
    *opaque = Encoder_Interface_init(0);
}

void dashcdg_amr_nb_encoder_destroy(void *opaque) {
    if (opaque != NULL) {
        Encoder_Interface_exit(opaque);
    }
}

int dashcdg_amr_nb_encoder_run(void *opaque, const int16_t *pcm48_960, uint8_t *out, size_t out_cap) {
    Word16 pcm8k[DASHCDG_AMR_NB_PCM8K];
    int n;

    if (opaque == NULL || pcm48_960 == NULL || out == NULL || out_cap < 32U) {
        return 0;
    }
    dashcdg_downsample_48k_to_8k_mono(pcm48_960, pcm8k);
    n = Encoder_Interface_Encode(opaque, MR122, pcm8k, out, 0);
    if (n <= 0 || (size_t) n > out_cap) {
        return 0;
    }
    return n;
}

void dashcdg_amr_nb_decoder_create(void **opaque) {
    if (opaque == NULL) {
        return;
    }
    *opaque = Decoder_Interface_init();
}

void dashcdg_amr_nb_decoder_destroy(void *opaque) {
    if (opaque != NULL) {
        Decoder_Interface_exit(opaque);
    }
}

int dashcdg_amr_nb_decoder_run(void *opaque, const uint8_t *in, size_t in_len, int16_t *pcm48_960, size_t pcm_cap_samples) {
    Word16 pcm8k[DASHCDG_AMR_NB_PCM8K];

    if (opaque == NULL || in == NULL || in_len == 0U || pcm48_960 == NULL || pcm_cap_samples < DASHCDG_AMR_NB_PCM48K) {
        return 0;
    }
    if (in_len > 64U) {
        return 0;
    }
    {
        unsigned char bits[64];
        memcpy(bits, in, in_len);
        Decoder_Interface_Decode(opaque, bits, pcm8k, 0);
    }
    dashcdg_upsample_8k_to_48k_mono(pcm8k, pcm48_960);
    return (int) DASHCDG_AMR_NB_PCM48K;
}
