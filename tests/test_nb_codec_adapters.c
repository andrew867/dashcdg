#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dashcdg/nb_codec_adapters.h"

#define DASHCDG_TEST_PCM48_FRAMES 960U
#define DASHCDG_TEST_PCM48_STEREO_SAMPLES (DASHCDG_TEST_PCM48_FRAMES * 2U)

static void dashcdg_fill_test_stereo_program(int16_t *pcm, size_t frames) {
    size_t i;

    for (i = 0U; i < frames; ++i) {
        double t = (double) i / 48000.0;
        double left = 14000.0 * sin(2.0 * 3.141592653589793 * 220.0 * t)
                + 6000.0 * sin(2.0 * 3.141592653589793 * 660.0 * t);
        double right = 12000.0 * sin(2.0 * 3.141592653589793 * 220.0 * t + 0.15)
                + 5000.0 * sin(2.0 * 3.141592653589793 * 880.0 * t);

        pcm[i * 2U] = (int16_t) left;
        pcm[i * 2U + 1U] = (int16_t) right;
    }
}

static void test_evrc_roundtrip_produces_pcm(void) {
    void *enc = NULL;
    void *dec = NULL;
    int16_t pcm_in[DASHCDG_TEST_PCM48_STEREO_SAMPLES];
    int16_t pcm_out[DASHCDG_TEST_PCM48_STEREO_SAMPLES];
    uint8_t packet[128];
    int encoded;
    int decoded;
    int64_t energy = 0;
    size_t i;

    dashcdg_fill_test_stereo_program(pcm_in, DASHCDG_TEST_PCM48_FRAMES);

    assert(dashcdg_evrc_encoder_create(&enc) == 1);
    assert(enc != NULL);
    assert(dashcdg_evrc_decoder_create(&dec) == 1);
    assert(dec != NULL);

    encoded = dashcdg_evrc_encode_pcm48_stereo_frame(
            enc,
            pcm_in,
            DASHCDG_TEST_PCM48_STEREO_SAMPLES,
            packet,
            sizeof(packet)
    );
    assert(encoded > 0);

    decoded = dashcdg_evrc_decode_to_pcm48_stereo(
            dec,
            packet,
            (size_t) encoded,
            pcm_out,
            DASHCDG_TEST_PCM48_STEREO_SAMPLES
    );
    assert(decoded == (int) DASHCDG_TEST_PCM48_FRAMES);

    for (i = 0U; i < DASHCDG_TEST_PCM48_STEREO_SAMPLES; ++i) {
        int32_t s = pcm_out[i];
        energy += (int64_t) s * (int64_t) s;
    }
    assert(energy > 1000000LL);

    dashcdg_evrc_decoder_destroy(dec);
    dashcdg_evrc_encoder_destroy(enc);
}

static void test_bluetooth_sbc_roundtrip_produces_pcm(void) {
    void *enc = NULL;
    void *dec = NULL;
    int16_t pcm_in[DASHCDG_TEST_PCM48_STEREO_SAMPLES];
    int16_t pcm_out[DASHCDG_TEST_PCM48_STEREO_SAMPLES];
    uint8_t packet[512];
    int encoded;
    int decoded;
    int64_t energy = 0;
    size_t i;

    dashcdg_fill_test_stereo_program(pcm_in, DASHCDG_TEST_PCM48_FRAMES);

    assert(dashcdg_bt_sbc_encoder_create(&enc) == 1);
    assert(enc != NULL);
    assert(dashcdg_bt_sbc_decoder_create(&dec) == 1);
    assert(dec != NULL);

    encoded = dashcdg_bt_sbc_encode_pcm48_stereo_frame(
            enc,
            pcm_in,
            DASHCDG_TEST_PCM48_STEREO_SAMPLES,
            packet,
            sizeof(packet)
    );
    assert(encoded > 1);

    decoded = dashcdg_bt_sbc_decode_to_pcm48_stereo(
            dec,
            packet,
            (size_t) encoded,
            pcm_out,
            DASHCDG_TEST_PCM48_STEREO_SAMPLES
    );
    if (decoded != (int) DASHCDG_TEST_PCM48_FRAMES) {
        fprintf(stderr, "sbc roundtrip mismatch: encoded=%d decoded=%d frames=%u\n",
                encoded,
                decoded,
                encoded > 0 ? (unsigned int) packet[0] : 0U);
    }
    assert(decoded == (int) DASHCDG_TEST_PCM48_FRAMES);

    for (i = 0U; i < DASHCDG_TEST_PCM48_STEREO_SAMPLES; ++i) {
        int32_t s = pcm_out[i];
        energy += (int64_t) s * (int64_t) s;
    }
    assert(energy > 1000000LL);

    dashcdg_bt_sbc_decoder_destroy(dec);
    dashcdg_bt_sbc_encoder_destroy(enc);
}

int main(void) {
    test_evrc_roundtrip_produces_pcm();
    test_bluetooth_sbc_roundtrip_produces_pcm();
    return 0;
}
