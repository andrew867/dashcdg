/* Makefile sets -DDASHCDG_CPU_PRE_SSE2_MINIMP3 on mingw32 legacy/retro; minimp3 uses SSE2
 * intrinsics when __SSE2__ is set (common MSYS2 i686 default) which faults on Pentium III. */
#ifdef DASHCDG_CPU_PRE_SSE2_MINIMP3
#define MINIMP3_NO_SIMD
#endif
#define MINIMP3_IMPLEMENTATION

#include "dashcdg/desktop_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dashcdg/media_clock.h"
#include "dashcdg/net_compat.h"

#ifndef DASHCDG_DESKTOP_WIN32_WAVEOUT
#define DASHCDG_DESKTOP_WIN32_WAVEOUT 0
#endif

void dashcdg_desktop_audio_stop_stream(struct dashcdg_desktop_audio *audio);

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
static pthread_mutex_t g_dashcdg_pa_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_dashcdg_pa_refcount = 0;

static int dashcdg_pa_host_init(void) {
    PaError err;

    pthread_mutex_lock(&g_dashcdg_pa_mutex);
    if (g_dashcdg_pa_refcount == 0) {
        err = Pa_Initialize();
        if (err != paNoError) {
            fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
            pthread_mutex_unlock(&g_dashcdg_pa_mutex);
            return 0;
        }
    }
    g_dashcdg_pa_refcount++;
    pthread_mutex_unlock(&g_dashcdg_pa_mutex);
    return 1;
}

static void dashcdg_pa_host_deinit(void) {
    pthread_mutex_lock(&g_dashcdg_pa_mutex);
    if (g_dashcdg_pa_refcount > 0) {
        g_dashcdg_pa_refcount--;
        if (g_dashcdg_pa_refcount == 0) {
            Pa_Terminate();
        }
    }
    pthread_mutex_unlock(&g_dashcdg_pa_mutex);
}
#endif

#if DASHCDG_HAVE_PORTAUDIO || (defined(DASHCDG_DESKTOP_WIN32_WAVEOUT) && (DASHCDG_DESKTOP_WIN32_WAVEOUT) && defined(_WIN32))
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
static int dashcdg_pa_latency_ms_from_timeinfo(
        const PaStreamCallbackTimeInfo *time_info,
        int fallback_ms
) {
    double sec;

    if (time_info == NULL) {
        return fallback_ms;
    }
    sec = time_info->outputBufferDacTime - time_info->currentTime;
    if (sec < 0.0 || sec > 3.0) {
        return fallback_ms;
    }
    return (int) (sec * 1000.0 + 0.5);
}

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

        latency_ms = dashcdg_pa_latency_ms_from_timeinfo(time_info, 80);
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

    latency_ms = dashcdg_pa_latency_ms_from_timeinfo(time_info, 40);
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
    PaStreamParameters out_params;
    const PaDeviceInfo *dev_info;
    PaDeviceIndex out_dev;
    double host_latency_s;

    if (!dashcdg_pa_host_init()) {
        return 0;
    }

    sample_rate = audio->mode == DASHCDG_AUDIO_MODE_STREAM ? (int) audio->stream_sample_rate : audio->file_info.hz;
    channels = audio->mode == DASHCDG_AUDIO_MODE_STREAM ? (int) audio->stream_channels : audio->file_info.channels;
    if (sample_rate <= 0 || channels <= 0) {
        dashcdg_pa_host_deinit();
        return 0;
    }

    out_dev = Pa_GetDefaultOutputDevice();
    if (out_dev == paNoDevice) {
        fprintf(stderr, "PortAudio error: no default output device\n");
        dashcdg_pa_host_deinit();
        return 0;
    }

    dev_info = Pa_GetDeviceInfo(out_dev);
    if (dev_info == NULL) {
        dashcdg_pa_host_deinit();
        return 0;
    }

    if (channels > dev_info->maxOutputChannels) {
        fprintf(
                stderr,
                "PortAudio error: need %d output channels, device supports at most %d\n",
                channels,
                dev_info->maxOutputChannels
        );
        dashcdg_pa_host_deinit();
        return 0;
    }

    memset(&out_params, 0, sizeof(out_params));
    out_params.device = out_dev;
    out_params.channelCount = channels;
    out_params.sampleFormat = paInt16;
    /*
     * Pa_OpenDefaultStream uses the device's default *low* latency. On Windows (WASAPI shared)
     * that is often only ~10–20 ms of host buffering, which glitches when the UI thread is busy.
     * Network streaming uses the driver's high-latency default with a floor so the callback keeps
     * up under load; local file playback keeps low latency for scrubbing.
     */
    if (audio->mode == DASHCDG_AUDIO_MODE_STREAM) {
        host_latency_s = dev_info->defaultHighOutputLatency;
        if (host_latency_s < 0.08) {
            host_latency_s = 0.08;
        }
        if (host_latency_s > 0.25) {
            host_latency_s = 0.25;
        }
    } else {
        host_latency_s = dev_info->defaultLowOutputLatency;
        if (host_latency_s < 0.01) {
            host_latency_s = 0.01;
        }
    }
    out_params.suggestedLatency = host_latency_s;
    out_params.hostApiSpecificStreamInfo = NULL;

    err = Pa_OpenStream(
            &audio->stream,
            NULL,
            &out_params,
            (double) sample_rate,
            paFramesPerBufferUnspecified,
            0,
            dashcdg_pa_callback,
            audio
    );
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
        dashcdg_pa_host_deinit();
        return 0;
    }

    err = Pa_StartStream(audio->stream);
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(audio->stream);
        audio->stream = NULL;
        dashcdg_pa_host_deinit();
        return 0;
    }

    return 1;
}
#endif

#if DASHCDG_DESKTOP_WIN32_WAVEOUT && defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>

#ifndef WAVE_MAPPER
#define WAVE_MAPPER ((UINT) -1)
#endif

/* File playback: small chunks for scrub responsiveness. Network stream: larger chunks and more
 * queued buffers so the refill thread survives CPU spikes (legacy/slow hosts). */
#define DASHCDG_WINMM_BUFFERS_MAX 8U
#define DASHCDG_WINMM_FILE_CHUNK_MS 20U
#define DASHCDG_WINMM_STREAM_CHUNK_MS 40U
#define DASHCDG_WINMM_BUFFERS_FILE 4U
#define DASHCDG_WINMM_BUFFERS_STREAM 6U

struct dashcdg_winmm_ctx {
    HWAVEOUT hwo;
    HANDLE done_evt;
    pthread_t io_thread;
    int io_thread_created;
    volatile int stop;
    volatile int file_ended;
    WAVEHDR hdr[DASHCDG_WINMM_BUFFERS_MAX];
    int16_t *buffer_data;
    size_t chunk_frames;
    unsigned int buffer_count;
    unsigned int channels;
    unsigned int sample_rate;
};

static void dashcdg_winmm_fill_block(
        struct dashcdg_desktop_audio *audio,
        int16_t *dst,
        size_t frames,
        struct dashcdg_winmm_ctx *ctx
) {
    /* Approximate output delay: WinMM stream path uses ~40 ms chunks × 6 buffers (~240 ms peak queue). */
    int latency_ms = audio->mode == DASHCDG_AUDIO_MODE_STREAM ? 160 : 80;

    if (audio->mode == DASHCDG_AUDIO_MODE_STREAM) {
        (void) ctx;
        size_t total_samples;
        size_t consumed_frames;
        int audio_ts;

        if (audio->stream_channels == 0 || audio->stream_sample_rate == 0) {
            return;
        }

        consumed_frames = dashcdg_stream_buffer_consume(audio, (unsigned long) frames, dst);
        total_samples = frames * (size_t) audio->stream_channels;
        if (consumed_frames > 0U) {
            audio->stream_played_frames += consumed_frames;
        }
        if (DASHCDG_ATOMIC_GET(audio->stream_muted)) {
            memset(dst, 0, total_samples * sizeof(int16_t));
        }
        if (audio->stream_base_timestamp_ms >= 0) {
            audio_ts = (int) (audio->stream_base_timestamp_ms +
                    ((int64_t) audio->stream_played_frames * 1000LL) / (int64_t) audio->stream_sample_rate);
            audio_ts -= latency_ms;
            DASHCDG_ATOMIC_SET(audio->timestamp_ms, audio_ts < 0 ? 0 : audio_ts);
        }
        return;
    }

    if (audio->pcm == NULL) {
        return;
    }
    {
        int seek_to_sample = DASHCDG_ATOMIC_GET(audio->seek_to_sample);

        if (seek_to_sample >= 0) {
            if ((size_t) seek_to_sample > audio->pcm->size) {
                audio->pcm->index = audio->pcm->size;
            } else {
                audio->pcm->index = (size_t) seek_to_sample;
            }
            DASHCDG_ATOMIC_SET(audio->seek_to_sample, -1);
        }
    }
    {
        int audio_ts = dashcdg_desktop_audio_get_pos_ms(audio) - latency_ms;

        DASHCDG_ATOMIC_SET(audio->timestamp_ms, audio_ts < 0 ? 0 : audio_ts);
    }

    dashcdg_pcm_buffer_consume(
            audio->pcm,
            frames * (unsigned long) audio->file_info.channels,
            (uint16_t *) dst
    );
    if (DASHCDG_ATOMIC_GET(audio->stream_muted)) {
        memset(dst, 0, frames * (unsigned long) audio->file_info.channels * sizeof(int16_t));
    }

    if (audio->pcm->index >= audio->pcm->size) {
        ctx->file_ended = 1;
    }
}

static void *dashcdg_winmm_thread_main(void *arg) {
    struct dashcdg_desktop_audio *audio = (struct dashcdg_desktop_audio *) arg;
    struct dashcdg_winmm_ctx *ctx = (struct dashcdg_winmm_ctx *) audio->audio_io_ctx;
    size_t cf;
    unsigned int ch;
    unsigned int i;
    unsigned int nbuf;
    MMRESULT mr;

    if (ctx == NULL) {
        return NULL;
    }

    (void) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    cf = ctx->chunk_frames;
    ch = ctx->channels;
    nbuf = ctx->buffer_count;

    for (i = 0U; i < nbuf; ++i) {
        ctx->hdr[i].dwBufferLength = (DWORD) (cf * (size_t) ch * sizeof(int16_t));
        ctx->hdr[i].lpData = (LPSTR) (ctx->buffer_data + ((size_t) i * cf * (size_t) ch));
        ctx->hdr[i].dwUser = (DWORD_PTR) i;
        mr = waveOutPrepareHeader(ctx->hwo, &ctx->hdr[i], sizeof(WAVEHDR));
        if (mr != MMSYSERR_NOERROR) {
            ctx->stop = 1;
            return NULL;
        }
        dashcdg_winmm_fill_block(audio, (int16_t *) ctx->hdr[i].lpData, cf, ctx);
        mr = waveOutWrite(ctx->hwo, &ctx->hdr[i], sizeof(WAVEHDR));
        if (mr != MMSYSERR_NOERROR) {
            ctx->stop = 1;
            return NULL;
        }
    }

    while (!ctx->stop) {
        int progressed = 0;

        if (ctx->file_ended && audio->mode == DASHCDG_AUDIO_MODE_FILE) {
            break;
        }

        for (i = 0U; i < nbuf; ++i) {
            if ((ctx->hdr[i].dwFlags & WHDR_DONE) != 0U) {
                progressed = 1;
                if (ctx->file_ended && audio->mode == DASHCDG_AUDIO_MODE_FILE) {
                    ctx->stop = 1;
                    break;
                }
                dashcdg_winmm_fill_block(audio, (int16_t *) ctx->hdr[i].lpData, cf, ctx);
                mr = waveOutWrite(ctx->hwo, &ctx->hdr[i], sizeof(WAVEHDR));
                if (mr != MMSYSERR_NOERROR) {
                    ctx->stop = 1;
                    break;
                }
            }
        }

        if (ctx->stop) {
            break;
        }

        if (progressed == 0) {
            (void) WaitForSingleObject(ctx->done_evt, audio->mode == DASHCDG_AUDIO_MODE_STREAM ? 80U : 25U);
        }
    }

    (void) waveOutReset(ctx->hwo);
    for (i = 0U; i < nbuf; ++i) {
        (void) waveOutUnprepareHeader(ctx->hwo, &ctx->hdr[i], sizeof(WAVEHDR));
    }
    (void) waveOutClose(ctx->hwo);
    ctx->hwo = NULL;
    DASHCDG_ATOMIC_SET(audio->playback_running, 0);
    return NULL;
}

static void CALLBACK dashcdg_winmm_callback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    struct dashcdg_winmm_ctx *ctx = (struct dashcdg_winmm_ctx *) dwInstance;

    (void) hwo;
    (void) dwParam1;
    (void) dwParam2;
    if (uMsg != WOM_DONE || ctx == NULL || ctx->done_evt == NULL) {
        return;
    }
    (void) SetEvent(ctx->done_evt);
}

static int dashcdg_winmm_create_stream(struct dashcdg_desktop_audio *audio) {
    struct dashcdg_winmm_ctx *ctx;
    WAVEFORMATEX wfx;
    MMRESULT mr;
    int sample_rate;
    int channels;

    ctx = (struct dashcdg_winmm_ctx *) calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return 0;
    }

    sample_rate = audio->mode == DASHCDG_AUDIO_MODE_STREAM ? (int) audio->stream_sample_rate : audio->file_info.hz;
    channels = audio->mode == DASHCDG_AUDIO_MODE_STREAM ? (int) audio->stream_channels : audio->file_info.channels;
    if (sample_rate <= 0 || channels <= 0) {
        free(ctx);
        return 0;
    }

    ctx->sample_rate = (unsigned int) sample_rate;
    ctx->channels = (unsigned int) channels;
    if (audio->mode == DASHCDG_AUDIO_MODE_STREAM) {
        ctx->buffer_count = DASHCDG_WINMM_BUFFERS_STREAM;
        ctx->chunk_frames = ((unsigned long long) (unsigned int) sample_rate * (unsigned long long) DASHCDG_WINMM_STREAM_CHUNK_MS) / 1000ULL;
    } else {
        ctx->buffer_count = DASHCDG_WINMM_BUFFERS_FILE;
        ctx->chunk_frames = ((unsigned long long) (unsigned int) sample_rate * (unsigned long long) DASHCDG_WINMM_FILE_CHUNK_MS) / 1000ULL;
    }
    if (ctx->chunk_frames < 1U) {
        ctx->chunk_frames = 1U;
    }

    ctx->buffer_data = (int16_t *) calloc((size_t) ctx->buffer_count * ctx->chunk_frames * (size_t) channels, sizeof(int16_t));
    if (ctx->buffer_data == NULL) {
        free(ctx);
        return 0;
    }

    ctx->done_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (ctx->done_evt == NULL) {
        free(ctx->buffer_data);
        free(ctx);
        return 0;
    }

    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD) channels;
    wfx.nSamplesPerSec = (DWORD) sample_rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD) ((channels * 16) / 8);
    wfx.nAvgBytesPerSec = (DWORD) sample_rate * (DWORD) wfx.nBlockAlign;
    wfx.cbSize = 0;

    mr = waveOutOpen(
            &ctx->hwo,
            WAVE_MAPPER,
            &wfx,
            (DWORD_PTR) dashcdg_winmm_callback,
            (DWORD_PTR) ctx,
            CALLBACK_FUNCTION
    );
    if (mr != MMSYSERR_NOERROR) {
        (void) CloseHandle(ctx->done_evt);
        free(ctx->buffer_data);
        free(ctx);
        fprintf(stderr, "waveOutOpen failed: %u\n", (unsigned int) mr);
        return 0;
    }

    audio->audio_io_ctx = ctx;

    if (pthread_create(&ctx->io_thread, NULL, dashcdg_winmm_thread_main, audio) != 0) {
        (void) waveOutClose(ctx->hwo);
        ctx->hwo = NULL;
        (void) CloseHandle(ctx->done_evt);
        ctx->done_evt = NULL;
        free(ctx->buffer_data);
        free(ctx);
        audio->audio_io_ctx = NULL;
        return 0;
    }
    ctx->io_thread_created = 1;
    return 1;
}

static void dashcdg_winmm_destroy_stream(struct dashcdg_desktop_audio *audio) {
    struct dashcdg_winmm_ctx *ctx;

    if (audio == NULL) {
        return;
    }
    ctx = (struct dashcdg_winmm_ctx *) audio->audio_io_ctx;
    if (ctx == NULL) {
        return;
    }

    ctx->stop = 1;
    if (ctx->io_thread_created) {
        (void) pthread_join(ctx->io_thread, NULL);
        ctx->io_thread_created = 0;
    }

    if (ctx->done_evt != NULL) {
        (void) CloseHandle(ctx->done_evt);
        ctx->done_evt = NULL;
    }

    free(ctx->buffer_data);
    free(ctx);
    audio->audio_io_ctx = NULL;
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

    dashcdg_desktop_audio_stop_stream(audio);

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

    dashcdg_desktop_audio_stop_stream(audio);

    DASHCDG_ATOMIC_SET(audio->playback_running, 0);
    return 1;
#elif DASHCDG_DESKTOP_WIN32_WAVEOUT && defined(_WIN32)
    DASHCDG_ATOMIC_SET(audio->playback_running, 1);
    if (!dashcdg_winmm_create_stream(audio)) {
        DASHCDG_ATOMIC_SET(audio->playback_running, 0);
        return 0;
    }

    while (DASHCDG_ATOMIC_GET(audio->playback_running)) {
        dashcdg_sleep_ms(50U);
    }

    dashcdg_desktop_audio_stop_stream(audio);
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
#if DASHCDG_HAVE_PORTAUDIO
    if (!dashcdg_desktop_audio_create_stream(audio)) {
        DASHCDG_ATOMIC_SET(audio->playback_running, 0);
        return 0;
    }
#elif DASHCDG_DESKTOP_WIN32_WAVEOUT && defined(_WIN32)
    if (!dashcdg_winmm_create_stream(audio)) {
        DASHCDG_ATOMIC_SET(audio->playback_running, 0);
        return 0;
    }
#else
    DASHCDG_ATOMIC_SET(audio->playback_running, 0);
    return 0;
#endif

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
        dashcdg_pa_host_deinit();
    }
#elif DASHCDG_DESKTOP_WIN32_WAVEOUT && defined(_WIN32)
    dashcdg_winmm_destroy_stream(audio);
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
