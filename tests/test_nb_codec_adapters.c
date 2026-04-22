#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dashcdg/amr_codec.h"
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

static void dashcdg_fill_test_mono_program(int16_t *pcm, size_t frames, double phase) {
    size_t i;

    for (i = 0U; i < frames; ++i) {
        double t = (double) i / 48000.0;
        double s = 15000.0 * sin(2.0 * 3.141592653589793 * 220.0 * t + phase)
                + 7000.0 * sin(2.0 * 3.141592653589793 * 1100.0 * t + phase * 0.5);

        pcm[i] = (int16_t) s;
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

static void test_qcelp8k_roundtrip_produces_pcm(void) {
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

    assert(dashcdg_qcelp8k_encoder_create(&enc) == 1);
    assert(enc != NULL);
    assert(dashcdg_qcelp8k_decoder_create(&dec) == 1);
    assert(dec != NULL);

    encoded = dashcdg_qcelp8k_encode_pcm48_stereo_frame(
            enc,
            pcm_in,
            DASHCDG_TEST_PCM48_STEREO_SAMPLES,
            packet,
            sizeof(packet)
    );
    assert(encoded > 0);

    decoded = dashcdg_qcelp8k_decode_to_pcm48_stereo(
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

    dashcdg_qcelp8k_decoder_destroy(dec);
    dashcdg_qcelp8k_encoder_destroy(enc);
}

static void test_amr_wb_roundtrip_produces_pcm(void) {
    void *enc = NULL;
    void *dec = NULL;
    int16_t pcm_in[DASHCDG_TEST_PCM48_FRAMES];
    int16_t pcm_out[DASHCDG_TEST_PCM48_FRAMES];
    uint8_t packet[128];
    int encoded;
    int decoded;
    int64_t energy = 0;
    size_t i;

    dashcdg_fill_test_mono_program(pcm_in, DASHCDG_TEST_PCM48_FRAMES, 0.2);

    dashcdg_amr_wb_encoder_create(&enc);
    dashcdg_amr_wb_decoder_create(&dec);
    assert(enc != NULL);
    assert(dec != NULL);

    encoded = dashcdg_amr_wb_encoder_run(enc, pcm_in, packet, sizeof(packet));
    assert(encoded > 0);

    decoded = dashcdg_amr_wb_decoder_run(dec, packet, (size_t) encoded, pcm_out, DASHCDG_TEST_PCM48_FRAMES);
    assert(decoded == (int) DASHCDG_TEST_PCM48_FRAMES);

    for (i = 0U; i < DASHCDG_TEST_PCM48_FRAMES; ++i) {
        int32_t s = pcm_out[i];
        energy += (int64_t) s * (int64_t) s;
    }
    assert(energy > 1000000LL);

    dashcdg_amr_wb_decoder_destroy(dec);
    dashcdg_amr_wb_encoder_destroy(enc);
}

static void test_amr_nb_roundtrip_produces_pcm(void) {
    void *enc = NULL;
    void *dec = NULL;
    int16_t pcm_in[DASHCDG_TEST_PCM48_FRAMES];
    int16_t pcm_out[DASHCDG_TEST_PCM48_FRAMES];
    uint8_t packet[64];
    int encoded;
    int decoded;
    int64_t energy = 0;
    size_t i;

    dashcdg_fill_test_mono_program(pcm_in, DASHCDG_TEST_PCM48_FRAMES, 0.4);

    dashcdg_amr_nb_encoder_create(&enc);
    dashcdg_amr_nb_decoder_create(&dec);
    assert(enc != NULL);
    assert(dec != NULL);

    encoded = dashcdg_amr_nb_encoder_run(enc, pcm_in, packet, sizeof(packet));
    assert(encoded > 0);

    decoded = dashcdg_amr_nb_decoder_run(dec, packet, (size_t) encoded, pcm_out, DASHCDG_TEST_PCM48_FRAMES);
    assert(decoded == (int) DASHCDG_TEST_PCM48_FRAMES);

    for (i = 0U; i < DASHCDG_TEST_PCM48_FRAMES; ++i) {
        int32_t s = pcm_out[i];
        energy += (int64_t) s * (int64_t) s;
    }
    assert(energy > 1000000LL);

    dashcdg_amr_nb_decoder_destroy(dec);
    dashcdg_amr_nb_encoder_destroy(enc);
}

static void test_evrc_repeated_recreate_roundtrip_stays_stable(void) {
    int16_t pcm_in[DASHCDG_TEST_PCM48_STEREO_SAMPLES];
    int16_t pcm_out[DASHCDG_TEST_PCM48_STEREO_SAMPLES];
    uint8_t packet[128];
    int cycle;

    dashcdg_fill_test_stereo_program(pcm_in, DASHCDG_TEST_PCM48_FRAMES);

    for (cycle = 0; cycle < 8; ++cycle) {
        void *enc = NULL;
        void *dec = NULL;
        int encoded;
        int decoded;
        int64_t energy = 0;
        size_t i;

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
}

static void test_amr_nb_encoders_keep_independent_overlap_state(void) {
    int16_t frame_a[DASHCDG_TEST_PCM48_FRAMES];
    int16_t frame_b[DASHCDG_TEST_PCM48_FRAMES];
    int16_t frame_c[DASHCDG_TEST_PCM48_FRAMES];
    uint8_t seq_pkt1[64];
    uint8_t seq_pkt2[64];
    uint8_t alt_pkt2[64];
    uint8_t scratch_pkt[64];
    void *seq = NULL;
    void *a = NULL;
    void *b = NULL;
    int seq_len1;
    int seq_len2;
    int alt_len2;

    dashcdg_fill_test_mono_program(frame_a, DASHCDG_TEST_PCM48_FRAMES, 0.1);
    dashcdg_fill_test_mono_program(frame_b, DASHCDG_TEST_PCM48_FRAMES, 0.6);
    dashcdg_fill_test_mono_program(frame_c, DASHCDG_TEST_PCM48_FRAMES, 1.1);

    dashcdg_amr_nb_encoder_create(&seq);
    dashcdg_amr_nb_encoder_create(&a);
    dashcdg_amr_nb_encoder_create(&b);
    assert(seq != NULL);
    assert(a != NULL);
    assert(b != NULL);

    seq_len1 = dashcdg_amr_nb_encoder_run(seq, frame_a, seq_pkt1, sizeof(seq_pkt1));
    seq_len2 = dashcdg_amr_nb_encoder_run(seq, frame_b, seq_pkt2, sizeof(seq_pkt2));
    assert(seq_len1 > 0);
    assert(seq_len2 > 0);

    assert(dashcdg_amr_nb_encoder_run(a, frame_a, scratch_pkt, sizeof(scratch_pkt)) > 0);
    assert(dashcdg_amr_nb_encoder_run(b, frame_c, scratch_pkt, sizeof(scratch_pkt)) > 0);
    alt_len2 = dashcdg_amr_nb_encoder_run(a, frame_b, alt_pkt2, sizeof(alt_pkt2));
    assert(alt_len2 > 0);

    assert(alt_len2 == seq_len2);
    assert(memcmp(seq_pkt2, alt_pkt2, (size_t) seq_len2) == 0);

    dashcdg_amr_nb_encoder_destroy(b);
    dashcdg_amr_nb_encoder_destroy(a);
    dashcdg_amr_nb_encoder_destroy(seq);
}

static void test_amr_wb_encoders_keep_independent_overlap_state(void) {
    int16_t frame_a[DASHCDG_TEST_PCM48_FRAMES];
    int16_t frame_b[DASHCDG_TEST_PCM48_FRAMES];
    int16_t frame_c[DASHCDG_TEST_PCM48_FRAMES];
    uint8_t seq_pkt1[128];
    uint8_t seq_pkt2[128];
    uint8_t alt_pkt2[128];
    uint8_t scratch_pkt[128];
    void *seq = NULL;
    void *a = NULL;
    void *b = NULL;
    int seq_len1;
    int seq_len2;
    int alt_len2;

    dashcdg_fill_test_mono_program(frame_a, DASHCDG_TEST_PCM48_FRAMES, 0.15);
    dashcdg_fill_test_mono_program(frame_b, DASHCDG_TEST_PCM48_FRAMES, 0.7);
    dashcdg_fill_test_mono_program(frame_c, DASHCDG_TEST_PCM48_FRAMES, 1.3);

    dashcdg_amr_wb_encoder_create(&seq);
    dashcdg_amr_wb_encoder_create(&a);
    dashcdg_amr_wb_encoder_create(&b);
    assert(seq != NULL);
    assert(a != NULL);
    assert(b != NULL);

    seq_len1 = dashcdg_amr_wb_encoder_run(seq, frame_a, seq_pkt1, sizeof(seq_pkt1));
    seq_len2 = dashcdg_amr_wb_encoder_run(seq, frame_b, seq_pkt2, sizeof(seq_pkt2));
    assert(seq_len1 > 0);
    assert(seq_len2 > 0);

    assert(dashcdg_amr_wb_encoder_run(a, frame_a, scratch_pkt, sizeof(scratch_pkt)) > 0);
    assert(dashcdg_amr_wb_encoder_run(b, frame_c, scratch_pkt, sizeof(scratch_pkt)) > 0);
    alt_len2 = dashcdg_amr_wb_encoder_run(a, frame_b, alt_pkt2, sizeof(alt_pkt2));
    assert(alt_len2 > 0);

    assert(alt_len2 == seq_len2);
    assert(memcmp(seq_pkt2, alt_pkt2, (size_t) seq_len2) == 0);

    dashcdg_amr_wb_encoder_destroy(b);
    dashcdg_amr_wb_encoder_destroy(a);
    dashcdg_amr_wb_encoder_destroy(seq);
}

int main(void) {
    test_amr_wb_roundtrip_produces_pcm();
    test_amr_nb_roundtrip_produces_pcm();
    test_evrc_roundtrip_produces_pcm();
    test_qcelp8k_roundtrip_produces_pcm();
    test_bluetooth_sbc_roundtrip_produces_pcm();
    test_evrc_repeated_recreate_roundtrip_stays_stable();
    test_amr_nb_encoders_keep_independent_overlap_state();
    test_amr_wb_encoders_keep_independent_overlap_state();
    return 0;
}
