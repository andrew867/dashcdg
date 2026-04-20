#ifndef DASHCDG_DESKTOP_AUDIO_H
#define DASHCDG_DESKTOP_AUDIO_H

#include <pthread.h>
#include <stdint.h>

#if defined(DASHCDG_DESKTOP_WIN32_WAVEOUT) && (DASHCDG_DESKTOP_WIN32_WAVEOUT)
#undef DASHCDG_HAVE_PORTAUDIO
#define DASHCDG_HAVE_PORTAUDIO 0
typedef void PaStream;
#elif defined(__has_include)
#if __has_include(<portaudio.h>)
#include <portaudio.h>
#define DASHCDG_HAVE_PORTAUDIO 1
#else
#define DASHCDG_HAVE_PORTAUDIO 0
typedef void PaStream;
#endif
#else
#include <portaudio.h>
#define DASHCDG_HAVE_PORTAUDIO 1
#endif

#if defined(DASHCDG_CPU_PRE_SSE2_MINIMP3) && !defined(MINIMP3_NO_SIMD)
#define MINIMP3_NO_SIMD
#endif
#include "minimp3_ex.h"

struct dashcdg_pcm_buffer {
    size_t size;
    size_t index;
    uint16_t *buffer;
};

struct dashcdg_desktop_audio {
    struct dashcdg_pcm_buffer *pcm;
    mp3dec_t decoder;
    mp3dec_file_info_t file_info;
    mp3dec_ex_t stream_decoder;
    PaStream *stream;
    void *audio_io_ctx;
    pthread_t thread;
    int timestamp_ms;
    int seek_to_sample;
    uint64_t playback_started_ms;
    int playback_running;
    int mode;
    /* Stream multicast: decoded PCM/sample clock from session_info; playback ring + device use stream_sample_rate. */
    uint32_t session_sample_rate;
    uint32_t stream_sample_rate;
    uint16_t stream_channels;
    int64_t stream_base_timestamp_ms;
    uint64_t stream_played_frames;
    size_t stream_capacity_frames;
    size_t stream_read_frame;
    size_t stream_write_frame;
    size_t stream_queued_frames;
    int16_t *stream_pcm;
    int stream_decoder_open;
    int stream_muted;
    pthread_mutex_t stream_mutex;
};

struct dashcdg_desktop_audio *dashcdg_desktop_audio_new(void);
void dashcdg_desktop_audio_free(struct dashcdg_desktop_audio *audio);
int dashcdg_desktop_audio_load_file(struct dashcdg_desktop_audio *audio, const char *path);
int dashcdg_desktop_audio_open_mp3_stream(struct dashcdg_desktop_audio *audio, const char *path);
int dashcdg_desktop_audio_seek_mp3_stream(struct dashcdg_desktop_audio *audio, uint32_t seek_ms);
void dashcdg_desktop_audio_close_mp3_stream(struct dashcdg_desktop_audio *audio);
size_t dashcdg_desktop_audio_read_mp3_frames(
        struct dashcdg_desktop_audio *audio,
        int16_t *pcm,
        size_t max_frames,
        uint32_t *sample_rate,
        uint16_t *channels
);
int dashcdg_desktop_audio_get_pos_ms(struct dashcdg_desktop_audio *audio);
int dashcdg_desktop_audio_get_duration_ms(const struct dashcdg_desktop_audio *audio);
void dashcdg_desktop_audio_seek_ms(struct dashcdg_desktop_audio *audio, uint32_t ms);
int dashcdg_desktop_audio_is_running(const struct dashcdg_desktop_audio *audio);
int dashcdg_desktop_audio_play(struct dashcdg_desktop_audio *audio);
int dashcdg_desktop_audio_init_stream(
        struct dashcdg_desktop_audio *audio,
        uint32_t sample_rate,
        uint16_t channels,
        uint32_t buffer_ms
);
int dashcdg_desktop_audio_start_stream(struct dashcdg_desktop_audio *audio);
/*
 * After start_stream failed: last diagnostic string (may be empty if failure had no PaError).
 * Valid until the next open attempt. Always NUL-terminated.
 */
const char *dashcdg_desktop_audio_last_stream_open_error(void);
void dashcdg_desktop_audio_stop_stream(struct dashcdg_desktop_audio *audio);
size_t dashcdg_desktop_audio_queue_frames(
        struct dashcdg_desktop_audio *audio,
        const int16_t *pcm,
        size_t frame_count,
        int64_t first_frame_timestamp_ms
);
void dashcdg_desktop_audio_flush_stream_ring(struct dashcdg_desktop_audio *audio);
uint32_t dashcdg_desktop_audio_buffered_ms(const struct dashcdg_desktop_audio *audio);
uint32_t dashcdg_desktop_audio_output_latency_ms(const struct dashcdg_desktop_audio *audio);
void dashcdg_desktop_audio_set_muted(struct dashcdg_desktop_audio *audio, int muted);
int dashcdg_desktop_audio_is_muted(const struct dashcdg_desktop_audio *audio);
uint32_t dashcdg_desktop_audio_session_sample_rate(const struct dashcdg_desktop_audio *audio);
uint32_t dashcdg_desktop_audio_output_sample_rate(const struct dashcdg_desktop_audio *audio);
void dashcdg_desktop_audio_refresh_stream_sample_rate(struct dashcdg_desktop_audio *audio);
/*
 * Non-zero once PortAudio stream exists or WinMM waveOut is active. Until then, PCM may be queued
 * while the DAC is still opening (desktop-rx starts output asynchronously).
 */
int dashcdg_desktop_audio_output_device_ready(const struct dashcdg_desktop_audio *audio);

#endif
