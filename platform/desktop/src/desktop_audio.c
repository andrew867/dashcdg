#define MINIMP3_IMPLEMENTATION

#include "dashcdg/desktop_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dashcdg/media_clock.h"
#include "dashcdg/net_compat.h"

#define DASHCDG_ATOMIC_GET(value) (__atomic_load_n(&(value), __ATOMIC_RELAXED))
#define DASHCDG_ATOMIC_SET(value, next) __atomic_store_n(&(value), (next), __ATOMIC_RELAXED)
#define DASHCDG_AUDIO_MODE_FILE 0
#define DASHCDG_AUDIO_MODE_STREAM 1

static struct dashcdg_pcm_buffer *dashcdg_pcm_buffer_new(uint16_t *buffer, size_t size) {
    struct dashcdg_pcm_buffer *pcm = (struct dashcdg_pcm_buffer *) malloc(sizeof(*pcm));

    if (pcm == NULL) {
        return NULL;
    }

    pcm->size = size;
    pcm->index = 0;
    pcm->buffer = buffer;
    return pcm;
}

static void dashcdg_pcm_buffer_free(struct dashcdg_pcm_buffer *pcm) {
    if (pcm == NULL) {
        return;
    }

    free(pcm->buffer);
    free(pcm);
}

#if DASHCDG_HAVE_PORTAUDIO
static size_t dashcdg_stream_buffer_consume(
        struct dashcdg_desktop_audio *audio,
        unsigned long frame_count,
        int16_t *out
) {
    size_t consumed = 0;
    size_t total_samples;

    if (audio == NULL || out == NULL || audio->stream_pcm == NULL || audio->stream_channels == 0) {
        return 0;
    }

    total_samples = (size_t) frame_count * (size_t) audio->stream_channels;
    memset(out, 0, total_samples * sizeof(int16_t));

    pthread_mutex_lock(&audio->stream_mutex);
    while (consumed < (size_t) frame_count && audio->stream_queued_frames > 0) {
        size_t copy_frames = audio->stream_queued_frames;

        if (copy_frames > (size_t) frame_count - consumed) {
            copy_frames = (size_t) frame_count - consumed;
        }
        if (copy_frames > audio->stream_capacity_frames - audio->stream_read_frame) {
            copy_frames = audio->stream_capacity_frames - audio->stream_read_frame;
        }

        memcpy(
                out + (consumed * (size_t) audio->stream_channels),
                audio->stream_pcm + (audio->stream_read_frame * (size_t) audio->stream_channels),
                copy_frames * (size_t) audio->stream_channels * sizeof(int16_t)
        );
        audio->stream_read_frame = (audio->stream_read_frame + copy_frames) % audio->stream_capacity_frames;
        audio->stream_queued_frames -= copy_frames;
        consumed += copy_frames;
    }
    pthread_mutex_unlock(&audio->stream_mutex);
    return consumed;
}

static void dashcdg_pcm_buffer_consume(struct dashcdg_pcm_buffer *pcm, size_t samples, uint16_t *out) {
    if (pcm == NULL || out == NULL) {
        return;
    }

    if (pcm->index + samples > pcm->size) {
        size_t available = pcm->size - pcm->index;

        if (available > 0) {
            memcpy(out, pcm->buffer + pcm->index, available * sizeof(uint16_t));
        }

        if (samples > available) {
            memset(out + available, 0, (samples - available) * sizeof(uint16_t));
        }

        pcm->index = pcm->size;
        return;
    }

    memcpy(out, pcm->buffer + pcm->index, samples * sizeof(uint16_t));
    pcm->index += samples;
}
#endif

#if DASHCDG_HAVE_PORTAUDIO
static int dashcdg_pa_callback(
        const void *input_buffer,
        void *output_buffer,
        unsigned long frame_count,
        const PaStreamCallbackTimeInfo *time_info,
        PaStreamCallbackFlags status_flags,
        void *user_data
) {
    struct dashcdg_desktop_audio *audio = (struct dashcdg_desktop_audio *) user_data;
    int seek_to_sample;
    int latency_ms;
    int audio_ts;
    size_t consumed_frames;

    (void) input_buffer;
    (void) status_flags;

    if (audio == NULL) {
        return paAbort;
    }

    if (audio->mode == DASHCDG_AUDIO_MODE_STREAM) {
        size_t total_samples;

        if (audio->stream_channels == 0 || audio->stream_sample_rate == 0) {
            return paAbort;
        }

        latency_ms = (int) ((time_info->outputBufferDacTime - time_info->currentTime) * 1000.0);
        consumed_frames = dashcdg_stream_buffer_consume(audio, frame_count, (int16_t *) output_buffer);
        total_samples = (size_t) frame_count * (size_t) audio->stream_channels;
        if (consumed_frames > 0) {
            audio->stream_played_frames += consumed_frames;
        }
        if (DASHCDG_ATOMIC_GET(audio->stream_muted)) {
            memset(output_buffer, 0, total_samples * sizeof(int16_t));
        }
        if (audio->stream_base_timestamp_ms >= 0) {
            audio_ts = (int) (audio->stream_base_timestamp_ms +
                    ((int64_t) audio->stream_played_frames * 1000LL) / (int64_t) audio->stream_sample_rate);
            audio_ts -= latency_ms;
            DASHCDG_ATOMIC_SET(audio->timestamp_ms, audio_ts < 0 ? 0 : audio_ts);
        }
        return paContinue;
    }

    if (audio->pcm == NULL) {
        return paAbort;
    }

    seek_to_sample = DASHCDG_ATOMIC_GET(audio->seek_to_sample);
    if (seek_to_sample >= 0) {
        if ((size_t) seek_to_sample > audio->pcm->size) {
            audio->pcm->index = audio->pcm->size;
        } else {
            audio->pcm->index = (size_t) seek_to_sample;
        }
        DASHCDG_ATOMIC_SET(audio->seek_to_sample, -1);
    }

    latency_ms = (int) ((time_info->outputBufferDacTime - time_info->currentTime) * 1000.0);
    audio_ts = dashcdg_desktop_audio_get_pos_ms(audio) - latency_ms;
    DASHCDG_ATOMIC_SET(audio->timestamp_ms, audio_ts < 0 ? 0 : audio_ts);

    dashcdg_pcm_buffer_consume(
            audio->pcm,
            frame_count * (unsigned long) audio->file_info.channels,
            (uint16_t *) output_buffer
    );
    if (DASHCDG_ATOMIC_GET(audio->stream_muted)) {
        memset(
                output_buffer,
                0,
                frame_count * (unsigned long) audio->file_info.channels * sizeof(uint16_t)
        );
    }

    if (audio->pcm->index >= audio->pcm->size) {
        return paComplete;
    }

    return paContinue;
}

static int dashcdg_desktop_audio_create_stream(struct dashcdg_desktop_audio *audio) {
    PaError err;
    int sample_rate;
    int channels;

    err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
        return 0;
    }

    sample_rate = audio->mode == DASHCDG_AUDIO_MODE_STREAM ? (int) audio->stream_sample_rate : audio->file_info.hz;
    channels = audio->mode == DASHCDG_AUDIO_MODE_STREAM ? (int) audio->stream_channels : audio->file_info.channels;
    err = Pa_OpenDefaultStream(
            &audio->stream,
            0,
            channels,
            paInt16,
            sample_rate,
            paFramesPerBufferUnspecified,
            dashcdg_pa_callback,
            audio
    );
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
        return 0;
    }

    err = Pa_StartStream(audio->stream);
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
        return 0;
    }

    return 1;
}
#endif

static size_t dashcdg_ms_to_sample_index(struct dashcdg_desktop_audio *audio, uint32_t ms) {
    float samples_per_ms;
    size_t sample_index;

    if (audio == NULL || audio->file_info.channels == 0 || audio->file_info.hz == 0) {
        return 0;
    }

    samples_per_ms = (float) audio->file_info.hz / 1000.0f;
    sample_index = (size_t) ((float) ms * samples_per_ms * (float) audio->file_info.channels);
    if (sample_index > audio->file_info.samples) {
        sample_index = audio->file_info.samples;
    }

    return sample_index;
}

struct dashcdg_desktop_audio *dashcdg_desktop_audio_new(void) {
    struct dashcdg_desktop_audio *audio = (struct dashcdg_desktop_audio *) malloc(sizeof(*audio));

    if (audio == NULL) {
        return NULL;
    }

    memset(audio, 0, sizeof(*audio));
    pthread_mutex_init(&audio->stream_mutex, NULL);
    audio->mode = DASHCDG_AUDIO_MODE_FILE;
    audio->timestamp_ms = -1;
    audio->seek_to_sample = -1;
    audio->stream_base_timestamp_ms = -1;
    audio->stream_decoder_open = 0;
    return audio;
}

void dashcdg_desktop_audio_free(struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return;
    }

#if DASHCDG_HAVE_PORTAUDIO
    if (audio->stream != NULL) {
        Pa_CloseStream(audio->stream);
        Pa_Terminate();
    }
#endif

    if (audio->pcm != NULL) {
        dashcdg_pcm_buffer_free(audio->pcm);
    }

    dashcdg_desktop_audio_close_mp3_stream(audio);
    free(audio->stream_pcm);
    pthread_mutex_destroy(&audio->stream_mutex);

    free(audio);
}

int dashcdg_desktop_audio_load_file(struct dashcdg_desktop_audio *audio, const char *path) {
    int err;

    if (audio == NULL || path == NULL) {
        return 0;
    }

    dashcdg_desktop_audio_close_mp3_stream(audio);
    mp3dec_init(&audio->decoder);
    err = mp3dec_load(&audio->decoder, path, &audio->file_info, NULL, NULL);
    if (err < 0) {
        fprintf(stderr, "failed to decode MP3: %d\n", err);
        return 0;
    }

    if (audio->pcm != NULL) {
        dashcdg_pcm_buffer_free(audio->pcm);
        audio->pcm = NULL;
    }

    audio->pcm = dashcdg_pcm_buffer_new((uint16_t *) audio->file_info.buffer, audio->file_info.samples);
    audio->mode = DASHCDG_AUDIO_MODE_FILE;
    return audio->pcm != NULL;
}

int dashcdg_desktop_audio_open_mp3_stream(struct dashcdg_desktop_audio *audio, const char *path) {
    int err;

    if (audio == NULL || path == NULL) {
        return 0;
    }

    dashcdg_desktop_audio_close_mp3_stream(audio);
    err = mp3dec_ex_open(&audio->stream_decoder, path, MP3D_DO_NOT_SCAN);
    if (err < 0) {
        fprintf(stderr, "failed to open streaming MP3 decoder: %d\n", err);
        memset(&audio->stream_decoder, 0, sizeof(audio->stream_decoder));
        return 0;
    }

    audio->stream_decoder_open = 1;
    return 1;
}

void dashcdg_desktop_audio_close_mp3_stream(struct dashcdg_desktop_audio *audio) {
    if (audio == NULL || !audio->stream_decoder_open) {
        return;
    }

    mp3dec_ex_close(&audio->stream_decoder);
    memset(&audio->stream_decoder, 0, sizeof(audio->stream_decoder));
    audio->stream_decoder_open = 0;
}

size_t dashcdg_desktop_audio_read_mp3_frames(
        struct dashcdg_desktop_audio *audio,
        int16_t *pcm,
        size_t max_frames,
        uint32_t *sample_rate,
        uint16_t *channels
) {
    size_t samples_read;
    int decoder_channels;

    if (audio == NULL || pcm == NULL || max_frames == 0U || !audio->stream_decoder_open) {
        return 0U;
    }

    decoder_channels = audio->stream_decoder.info.channels;
    if (decoder_channels <= 0) {
        return 0U;
    }

    if (sample_rate != NULL) {
        *sample_rate = (uint32_t) audio->stream_decoder.info.hz;
    }
    if (channels != NULL) {
        *channels = (uint16_t) decoder_channels;
    }

    samples_read = mp3dec_ex_read(
            &audio->stream_decoder,
            (mp3d_sample_t *) pcm,
            max_frames * (size_t) decoder_channels
    );
    return samples_read / (size_t) decoder_channels;
}

int dashcdg_desktop_audio_get_pos_ms(struct dashcdg_desktop_audio *audio) {
    float samples_per_ms;

    if (audio == NULL || audio->pcm == NULL || audio->file_info.channels == 0 || audio->file_info.hz == 0) {
        return 0;
    }

    samples_per_ms = (float) audio->file_info.hz / 1000.0f;
    return (int) ((float) audio->pcm->index / samples_per_ms / (float) audio->file_info.channels);
}

int dashcdg_desktop_audio_get_duration_ms(const struct dashcdg_desktop_audio *audio) {
    if (audio == NULL || audio->file_info.hz == 0 || audio->file_info.channels == 0) {
        return 0;
    }

    return (int) (((uint64_t) audio->file_info.samples * 1000ULL) /
            ((uint64_t) audio->file_info.hz * (uint64_t) audio->file_info.channels));
}

void dashcdg_desktop_audio_seek_ms(struct dashcdg_desktop_audio *audio, uint32_t ms) {
    if (audio == NULL) {
        return;
    }

    DASHCDG_ATOMIC_SET(audio->seek_to_sample, (int) dashcdg_ms_to_sample_index(audio, ms));
}

int dashcdg_desktop_audio_is_running(const struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return 0;
    }

    return DASHCDG_ATOMIC_GET(audio->playback_running);
}

int dashcdg_desktop_audio_play(struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return 0;
    }

#if DASHCDG_HAVE_PORTAUDIO
    DASHCDG_ATOMIC_SET(audio->playback_running, 1);
    if (!dashcdg_desktop_audio_create_stream(audio)) {
        DASHCDG_ATOMIC_SET(audio->playback_running, 0);
        return 0;
    }

    while (Pa_IsStreamActive(audio->stream) == 1) {
        Pa_Sleep(50);
    }

    if (audio->stream != NULL) {
        Pa_CloseStream(audio->stream);
        audio->stream = NULL;
        Pa_Terminate();
    }

    DASHCDG_ATOMIC_SET(audio->playback_running, 0);
    return 1;
#else
    uint64_t started_at_ms = dashcdg_clock_now_ms();
    uint32_t base_pos_ms = (uint32_t) dashcdg_desktop_audio_get_pos_ms(audio);
    uint32_t duration_ms = (uint32_t) dashcdg_desktop_audio_get_duration_ms(audio);

    DASHCDG_ATOMIC_SET(audio->playback_running, 1);

    while (audio->playback_running) {
        int seek_to_sample = DASHCDG_ATOMIC_GET(audio->seek_to_sample);
        uint64_t now_ms = dashcdg_clock_now_ms();
        uint32_t current_pos_ms;

        if (seek_to_sample >= 0) {
            audio->pcm->index = (size_t) seek_to_sample;
            base_pos_ms = (uint32_t) dashcdg_desktop_audio_get_pos_ms(audio);
            started_at_ms = now_ms;
            DASHCDG_ATOMIC_SET(audio->seek_to_sample, -1);
        }

        current_pos_ms = base_pos_ms + (uint32_t) (now_ms - started_at_ms);
        audio->pcm->index = dashcdg_ms_to_sample_index(audio, current_pos_ms);
        DASHCDG_ATOMIC_SET(audio->timestamp_ms, (int) current_pos_ms);

        if (current_pos_ms >= duration_ms || audio->pcm->index >= audio->pcm->size) {
            break;
        }

        dashcdg_sleep_ms(10);
    }

    DASHCDG_ATOMIC_SET(audio->playback_running, 0);
    return 1;
#endif
}

int dashcdg_desktop_audio_init_stream(
        struct dashcdg_desktop_audio *audio,
        uint32_t sample_rate,
        uint16_t channels,
        uint32_t buffer_ms
) {
    size_t capacity_frames;

    if (audio == NULL || sample_rate == 0 || channels == 0 || buffer_ms == 0) {
        return 0;
    }

    capacity_frames = ((size_t) sample_rate * (size_t) buffer_ms) / 1000U;
    if (capacity_frames < (size_t) sample_rate / 10U) {
        capacity_frames = (size_t) sample_rate / 10U;
    }

    free(audio->stream_pcm);
    audio->stream_pcm = (int16_t *) calloc(capacity_frames * (size_t) channels, sizeof(int16_t));
    if (audio->stream_pcm == NULL) {
        return 0;
    }

    audio->mode = DASHCDG_AUDIO_MODE_STREAM;
    audio->stream_sample_rate = sample_rate;
    audio->stream_channels = channels;
    audio->stream_capacity_frames = capacity_frames;
    audio->stream_read_frame = 0;
    audio->stream_write_frame = 0;
    audio->stream_queued_frames = 0;
    audio->stream_played_frames = 0;
    audio->stream_base_timestamp_ms = -1;
    DASHCDG_ATOMIC_SET(audio->timestamp_ms, -1);
    return 1;
}

int dashcdg_desktop_audio_start_stream(struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return 0;
    }

    audio->mode = DASHCDG_AUDIO_MODE_STREAM;
    DASHCDG_ATOMIC_SET(audio->playback_running, 1);
    if (!dashcdg_desktop_audio_create_stream(audio)) {
        DASHCDG_ATOMIC_SET(audio->playback_running, 0);
        return 0;
    }

    return 1;
}

void dashcdg_desktop_audio_stop_stream(struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return;
    }

#if DASHCDG_HAVE_PORTAUDIO
    if (audio->stream != NULL) {
        Pa_StopStream(audio->stream);
        Pa_CloseStream(audio->stream);
        audio->stream = NULL;
        Pa_Terminate();
    }
#endif
    DASHCDG_ATOMIC_SET(audio->playback_running, 0);
}

size_t dashcdg_desktop_audio_queue_frames(
        struct dashcdg_desktop_audio *audio,
        const int16_t *pcm,
        size_t frame_count,
        int64_t first_frame_timestamp_ms
) {
    size_t written = 0;

    if (audio == NULL || pcm == NULL || audio->stream_pcm == NULL || audio->stream_channels == 0) {
        return 0;
    }

    pthread_mutex_lock(&audio->stream_mutex);
    if (audio->stream_base_timestamp_ms < 0 && frame_count > 0 && audio->stream_queued_frames == 0) {
        audio->stream_base_timestamp_ms = first_frame_timestamp_ms;
    }

    while (written < frame_count && audio->stream_queued_frames < audio->stream_capacity_frames) {
        size_t free_frames = audio->stream_capacity_frames - audio->stream_queued_frames;
        size_t copy_frames = frame_count - written;

        if (copy_frames > free_frames) {
            copy_frames = free_frames;
        }
        if (copy_frames > audio->stream_capacity_frames - audio->stream_write_frame) {
            copy_frames = audio->stream_capacity_frames - audio->stream_write_frame;
        }

        memcpy(
                audio->stream_pcm + (audio->stream_write_frame * (size_t) audio->stream_channels),
                pcm + (written * (size_t) audio->stream_channels),
                copy_frames * (size_t) audio->stream_channels * sizeof(int16_t)
        );
        audio->stream_write_frame = (audio->stream_write_frame + copy_frames) % audio->stream_capacity_frames;
        audio->stream_queued_frames += copy_frames;
        written += copy_frames;
    }
    pthread_mutex_unlock(&audio->stream_mutex);
    return written;
}

uint32_t dashcdg_desktop_audio_buffered_ms(const struct dashcdg_desktop_audio *audio) {
    size_t queued_frames;

    if (audio == NULL || audio->stream_sample_rate == 0) {
        return 0;
    }

    pthread_mutex_lock((pthread_mutex_t *) &audio->stream_mutex);
    queued_frames = audio->stream_queued_frames;
    pthread_mutex_unlock((pthread_mutex_t *) &audio->stream_mutex);
    return (uint32_t) ((queued_frames * 1000U) / audio->stream_sample_rate);
}

void dashcdg_desktop_audio_set_muted(struct dashcdg_desktop_audio *audio, int muted) {
    if (audio == NULL) {
        return;
    }

    DASHCDG_ATOMIC_SET(audio->stream_muted, muted ? 1 : 0);
}

int dashcdg_desktop_audio_is_muted(const struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return 0;
    }

    return DASHCDG_ATOMIC_GET(audio->stream_muted) != 0;
}
