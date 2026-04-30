#ifndef DASHCDG_AMR_CODEC_H
#define DASHCDG_AMR_CODEC_H

#include <stddef.h>
#include <stdint.h>

/*
 * AMR-WB / AMR-NB wrappers around vendored 3GPP codec-amr (floating-point).
 *
 * Legacy desktop path: one 20 ms @ 48 kHz mono frame (960 samples) after decode+SRC.
 * Native path: codec-rate PCM — WB 16 kHz / 320 samples, NB 8 kHz / 160 samples per 20 ms.
 * Bitstreams fit v4 DASHCDG_MAX_AUDIO_FRAME_BYTES.
 */

void dashcdg_amr_wb_encoder_create(void **opaque);
void dashcdg_amr_wb_encoder_destroy(void *opaque);
int dashcdg_amr_wb_encoder_run(void *opaque, const int16_t *pcm48_960, uint8_t *out, size_t out_cap);

void dashcdg_amr_wb_decoder_create(void **opaque);
void dashcdg_amr_wb_decoder_destroy(void *opaque);
int dashcdg_amr_wb_decoder_run(void *opaque, const uint8_t *in, size_t in_len, int16_t *pcm48_960, size_t pcm_cap_samples);
/** Jitter advanced without a wire frame; run WB PLC so `D_IF` state matches the timeline (IF2 `bfi=_lost_frame`). */
int dashcdg_amr_wb_decoder_run_lost(void *opaque, int16_t *pcm48_960, size_t pcm_cap_samples);
/** 16 kHz mono, 320 samples (20 ms); no 48 kHz SRC. */
int dashcdg_amr_wb_decoder_run_native(void *opaque, const uint8_t *in, size_t in_len, int16_t *pcm16k_320, size_t pcm_cap_samples);
int dashcdg_amr_wb_decoder_run_lost_native(void *opaque, int16_t *pcm16k_320, size_t pcm_cap_samples);

void dashcdg_amr_nb_encoder_create(void **opaque);
void dashcdg_amr_nb_encoder_destroy(void *opaque);
int dashcdg_amr_nb_encoder_run(void *opaque, const int16_t *pcm48_960, uint8_t *out, size_t out_cap);

void dashcdg_amr_nb_decoder_create(void **opaque);
void dashcdg_amr_nb_decoder_destroy(void *opaque);
int dashcdg_amr_nb_decoder_run(void *opaque, const uint8_t *in, size_t in_len, int16_t *pcm48_960, size_t pcm_cap_samples);
/** 8 kHz mono, 160 samples (20 ms); no 48 kHz SRC. */
int dashcdg_amr_nb_decoder_run_native(void *opaque, const uint8_t *in, size_t in_len, int16_t *pcm8k_160, size_t pcm_cap_samples);
int dashcdg_amr_nb_decoder_run_lost_native(void *opaque, int16_t *pcm8k_160, size_t pcm_cap_samples);

#endif
