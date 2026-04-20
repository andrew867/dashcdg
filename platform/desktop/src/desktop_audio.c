/* Makefile sets -DDASHCDG_CPU_PRE_SSE2_MINIMP3 on mingw32 legacy/retro; minimp3 uses SSE2
 * intrinsics when __SSE2__ is set (common MSYS2 i686 default) which faults on Pentium III. */
#ifdef DASHCDG_CPU_PRE_SSE2_MINIMP3
#define MINIMP3_NO_SIMD
#endif
#define MINIMP3_IMPLEMENTATION

#include "dashcdg/desktop_audio.h"

#include <math.h>
#include <stdarg.h>
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
#if defined(_WIN32)
#include <pa_win_wasapi.h>
#endif
static pthread_mutex_t g_dashcdg_pa_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_dashcdg_pa_refcount = 0;
static char g_dashcdg_pa_last_stream_open_fail[256];

static void dashcdg_pa_clear_stream_open_fail(void) {
    g_dashcdg_pa_last_stream_open_fail[0] = '\0';
}

static void dashcdg_pa_log_stream_open_failf(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    (void) vsnprintf(
            g_dashcdg_pa_last_stream_open_fail,
            sizeof(g_dashcdg_pa_last_stream_open_fail),
            fmt,
            ap
    );
    va_end(ap);
    g_dashcdg_pa_last_stream_open_fail[sizeof(g_dashcdg_pa_last_stream_open_fail) - 1U] = '\0';
    fprintf(stderr, "PortAudio error: %s\n", g_dashcdg_pa_last_stream_open_fail);
    fflush(stderr);
}

static void dashcdg_pa_log_stream_open_paerr(PaError err, const char *ctx) {
    const char *t = Pa_GetErrorText(err);

    if (ctx != NULL && ctx[0] != '\0') {
        dashcdg_pa_log_stream_open_failf("%s [%s]", t, ctx);
    } else {
        dashcdg_pa_log_stream_open_failf("%s", t);
    }
}

static int dashcdg_pa_host_init(void) {
    PaError err;

    pthread_mutex_lock(&g_dashcdg_pa_mutex);
    if (g_dashcdg_pa_refcount == 0) {
        err = Pa_Initialize();
        if (err != paNoError) {
            dashcdg_pa_log_stream_open_paerr(err, "Pa_Initialize");
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

/*
 * Drop queued PCM and reset DAC timeline anchors. Caller must hold stream_mutex when the callback
 * can run or another thread may queue; safe before Pa_StartStream (no callback yet).
 */
static void dashcdg_desktop_audio_flush_stream_ring_locked(struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return;
    }
    audio->stream_read_frame = 0;
    audio->stream_write_frame = 0;
    audio->stream_queued_frames = 0;
    audio->stream_base_timestamp_ms = -1;
    audio->stream_played_frames = 0;
}

void dashcdg_desktop_audio_flush_stream_ring(struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return;
    }

    pthread_mutex_lock(&audio->stream_mutex);
    dashcdg_desktop_audio_flush_stream_ring_locked(audio);
    pthread_mutex_unlock(&audio->stream_mutex);
    DASHCDG_ATOMIC_SET(audio->timestamp_ms, -1);
}

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
        total_samples = (size_t) frame_count * (size_t) audio->stream_channels;
#if defined(_WIN32)
        {
            int16_t *i16b;
            float *fout;
            size_t i;

            if (audio->stream_pa_i16_scratch == NULL || total_samples == 0U) {
                return paAbort;
            }
            if (total_samples > audio->stream_pa_i16_scratch_samples) {
                /* PortAudio block larger than prealloc; play silence to stay real-time safe. */
                memset(output_buffer, 0, total_samples * sizeof(float));
                consumed_frames = 0U;
            } else {
                i16b = audio->stream_pa_i16_scratch;
                consumed_frames = dashcdg_stream_buffer_consume(audio, frame_count, i16b);
                fout = (float *) output_buffer;
                for (i = 0; i < total_samples; i++) {
                    fout[i] = (float) i16b[i] * (1.0f / 32768.0f);
                }
            }
            if (DASHCDG_ATOMIC_GET(audio->stream_muted)) {
                memset(output_buffer, 0, total_samples * sizeof(float));
            }
        }
#else
        consumed_frames = dashcdg_stream_buffer_consume(audio, frame_count, (int16_t *) output_buffer);
        if (DASHCDG_ATOMIC_GET(audio->stream_muted)) {
            memset(output_buffer, 0, total_samples * sizeof(int16_t));
        }
#endif
        if (audio->stream_base_timestamp_ms >= 0) {
            /*
             * Advance by the full PortAudio block size. Partial underrun still outputs silence for
             * the whole block; stream_played_frames must track DAC time so CDG/graphics (which
             * use timestamp_ms) stay aligned with what is heard.
             */
            audio->stream_played_frames += (size_t) frame_count;
            audio_ts = (int) (audio->stream_base_timestamp_ms +
                    ((int64_t) audio->stream_played_frames * 1000LL) / (int64_t) audio->stream_sample_rate);
            audio_ts -= latency_ms;
            DASHCDG_ATOMIC_SET(audio->timestamp_ms, audio_ts < 0 ? 0 : audio_ts);
        }
        (void) consumed_frames;
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

/*
 * On Windows, Pa_GetDefaultOutputDevice() may follow a different host API ordering than the
 * active playback endpoint users expect (e.g. DirectSound vs WASAPI). Prefer WASAPI's default
 * output when that API is present so desktop-rx routes to the same "default" device as other apps.
 */
static PaDeviceIndex dashcdg_pa_preferred_default_output_device(void) {
    PaHostApiIndex wasapi_host;
    const PaHostApiInfo *hai;

    wasapi_host = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
    if (wasapi_host < 0) {
        return Pa_GetDefaultOutputDevice();
    }
    hai = Pa_GetHostApiInfo(wasapi_host);
    if (hai == NULL || hai->defaultOutputDevice < 0) {
        return Pa_GetDefaultOutputDevice();
    }
    return (PaDeviceIndex) hai->defaultOutputDevice;
}

#if defined(_WIN32)
/*
 * WASAPI shared mode: when our rate/layout differs from the system mixer, ISimpleAudioVolume /
 * Audio Engine may accept the stream yet route nothing audible without the auto-converter.
 * (Observed with Realtek + multicast RX: OpenStream succeeds, HUD/ring healthy, silence.)
 */
static void dashcdg_pa_attach_wasapi_auto_convert_for_stream(
        PaStreamParameters *out_params,
        const PaDeviceInfo *dev_info,
        int network_stream_mode,
        PaWasapiStreamInfo *wasapi
) {
    const PaHostApiInfo *hai;

    if (out_params == NULL || dev_info == NULL || wasapi == NULL || !network_stream_mode) {
        return;
    }
    hai = Pa_GetHostApiInfo(dev_info->hostApi);
    if (hai == NULL || hai->type != paWASAPI) {
        return;
    }
    memset(wasapi, 0, sizeof(*wasapi));
    wasapi->size = sizeof(PaWasapiStreamInfo);
    wasapi->hostApiType = paWASAPI;
    wasapi->version = 1;
    wasapi->flags = paWinWasapiAutoConvert;
    out_params->hostApiSpecificStreamInfo = wasapi;
}
#endif

static int dashcdg_desktop_audio_create_stream(struct dashcdg_desktop_audio *audio) {
    PaError err;
    int sample_rate;
    int channels;
    PaStreamParameters out_params;
    const PaDeviceInfo *dev_info;
    PaDeviceIndex out_dev;
    double host_latency_s;
#if defined(_WIN32)
    PaWasapiStreamInfo wasapi_stream_info;
#endif

    dashcdg_pa_clear_stream_open_fail();

    /*
     * Recover from a leaked PaStream / refcount mismatch (e.g. retry after StartStream failure or
     * interrupted setup). Opening again without closing leaves the device exclusive or inflates
     * g_dashcdg_pa_refcount past stop_stream's single deinit.
     */
    if (audio->stream != NULL) {
        (void) Pa_StopStream(audio->stream);
        Pa_CloseStream(audio->stream);
        audio->stream = NULL;
        dashcdg_pa_host_deinit();
    }

    if (!dashcdg_pa_host_init()) {
        return 0;
    }

    sample_rate = audio->mode == DASHCDG_AUDIO_MODE_STREAM ? (int) audio->stream_sample_rate : audio->file_info.hz;
    channels = audio->mode == DASHCDG_AUDIO_MODE_STREAM ? (int) audio->stream_channels : audio->file_info.channels;
    if (sample_rate <= 0 || channels <= 0) {
        dashcdg_pa_log_stream_open_failf(
                "invalid sample rate or channel count (%d Hz, %d ch) — init_stream/device setup bug?",
                sample_rate,
                channels
        );
        dashcdg_pa_host_deinit();
        return 0;
    }

    out_dev = dashcdg_pa_preferred_default_output_device();
    if (out_dev == paNoDevice) {
        dashcdg_pa_log_stream_open_failf("%s", "no default output device (preferred/WASAPI or Pa_GetDefaultOutputDevice)");
        dashcdg_pa_host_deinit();
        return 0;
    }

    dev_info = Pa_GetDeviceInfo(out_dev);
    if (dev_info == NULL) {
        dashcdg_pa_log_stream_open_failf("%s", "Pa_GetDeviceInfo returned NULL for default output device");
        dashcdg_pa_host_deinit();
        return 0;
    }

    if (channels > dev_info->maxOutputChannels) {
        dashcdg_pa_log_stream_open_failf(
                "need %d output channels, device supports at most %d",
                channels,
                dev_info->maxOutputChannels
        );
        dashcdg_pa_host_deinit();
        return 0;
    }

    memset(&out_params, 0, sizeof(out_params));
    out_params.device = out_dev;
    out_params.channelCount = channels;
#if defined(_WIN32)
    if (audio->mode == DASHCDG_AUDIO_MODE_STREAM) {
        out_params.sampleFormat = paFloat32;
    } else {
        out_params.sampleFormat = paInt16;
    }
#else
    out_params.sampleFormat = paInt16;
#endif
    /*
     * Pa_OpenDefaultStream uses the device's default *low* latency. On Windows (WASAPI shared)
     * that is often only ~10–20 ms of host buffering, which glitches when the UI thread is busy.
     * Network streaming uses the driver's high-latency default with a floor so the callback keeps
     * up under load; local file playback keeps low latency for scrubbing.
     */
    if (audio->mode == DASHCDG_AUDIO_MODE_STREAM) {
        host_latency_s = dev_info->defaultHighOutputLatency;
        if (host_latency_s < 0.12) {
            host_latency_s = 0.12;
        }
        if (host_latency_s > 0.28) {
            host_latency_s = 0.28;
        }
    } else {
        host_latency_s = dev_info->defaultLowOutputLatency;
        if (host_latency_s < 0.01) {
            host_latency_s = 0.01;
        }
    }
    out_params.suggestedLatency = host_latency_s;
    out_params.hostApiSpecificStreamInfo = NULL;
#if defined(_WIN32)
    dashcdg_pa_attach_wasapi_auto_convert_for_stream(
            &out_params,
            dev_info,
            audio->mode == DASHCDG_AUDIO_MODE_STREAM,
            &wasapi_stream_info
    );
#endif

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
    if (err != paNoError && audio->mode == DASHCDG_AUDIO_MODE_STREAM) {
        int alt = (int) (dev_info->defaultSampleRate + 0.5);

        if (alt >= 8000 && alt <= 192000 && alt != sample_rate) {
            fprintf(
                    stderr,
                    "[desktop-audio] Pa_OpenStream @ %d Hz failed (%s); retrying @ %d Hz\n",
                    sample_rate,
                    Pa_GetErrorText(err),
                    alt
            );
            fflush(stderr);
            err = Pa_OpenStream(
                    &audio->stream,
                    NULL,
                    &out_params,
                    (double) alt,
                    paFramesPerBufferUnspecified,
                    0,
                    dashcdg_pa_callback,
                    audio
            );
        }
    }
    if (err != paNoError) {
        dashcdg_pa_log_stream_open_paerr(err, "Pa_OpenStream");
        dashcdg_pa_host_deinit();
        return 0;
    }

    if (audio->mode == DASHCDG_AUDIO_MODE_STREAM && dev_info != NULL) {
        const PaHostApiInfo *hai_api = Pa_GetHostApiInfo(dev_info->hostApi);

        fprintf(
                stdout,
                "[desktop-audio] output stream: PaDeviceIndex=%d api=%s host_pcm=%s device=\"%s\"%s\n",
                (int) out_dev,
                hai_api != NULL ? hai_api->name : "?",
#if defined(_WIN32)
                out_params.sampleFormat == paFloat32 ? "float32" : "int16",
#else
                "int16",
#endif
                dev_info->name != NULL ? dev_info->name : "?",
#if defined(_WIN32)
                out_params.hostApiSpecificStreamInfo != NULL ? " [WASAPI AutoConvert]" : ""
#else
                ""
#endif
        );
        fflush(stdout);
    }

    /*
     * Query the stream's actual sample rate before StartStream so we can discard pre-open PCM that
     * was queued at session decode rate when the DAC will tick at a different Hz (prevents mixed
     * clock material in the ring and audible pitch/SRC churn).
     */
    {
        const PaStreamInfo *psi = Pa_GetStreamInfo(audio->stream);

        if (psi != NULL && audio->mode == DASHCDG_AUDIO_MODE_STREAM) {
            double got = psi->sampleRate;
            uint32_t got_u = (uint32_t) (got + 0.5);

            pthread_mutex_lock(&audio->stream_mutex);
            audio->stream_sample_rate = got_u;
            if (fabs(got - (double) audio->session_sample_rate) > 0.5) {
                fprintf(
                        stderr,
                        "[desktop-audio] output device %.1f Hz vs session/decode %u Hz (resample in RX before queue; flushed pre-open ring).\n",
                        got,
                        (unsigned int) audio->session_sample_rate
                );
                fflush(stderr);
                dashcdg_desktop_audio_flush_stream_ring_locked(audio);
                DASHCDG_ATOMIC_SET(audio->timestamp_ms, -1);
            }
            pthread_mutex_unlock(&audio->stream_mutex);
        }
    }

    err = Pa_StartStream(audio->stream);
    if (err != paNoError) {
        dashcdg_pa_log_stream_open_paerr(err, "Pa_StartStream");
        Pa_CloseStream(audio->stream);
        audio->stream = NULL;
        dashcdg_pa_host_deinit();
        return 0;
    }

    dashcdg_pa_clear_stream_open_fail();
    return 1;
}

const char *dashcdg_desktop_audio_last_stream_open_error(void) {
    return g_dashcdg_pa_last_stream_open_fail;
}
#endif

#if !DASHCDG_HAVE_PORTAUDIO
const char *dashcdg_desktop_audio_last_stream_open_error(void) {
    return "";
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
        if (DASHCDG_ATOMIC_GET(audio->stream_muted)) {
            memset(dst, 0, total_samples * sizeof(int16_t));
        }
        if (audio->stream_base_timestamp_ms >= 0) {
            audio->stream_played_frames += frames;
            audio_ts = (int) (audio->stream_base_timestamp_ms +
                    ((int64_t) audio->stream_played_frames * 1000LL) / (int64_t) audio->stream_sample_rate);
            audio_ts -= latency_ms;
            DASHCDG_ATOMIC_SET(audio->timestamp_ms, audio_ts < 0 ? 0 : audio_ts);
        }
        (void) consumed_frames;
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
    int opened_rate;
    int fallback_index;
    static const int fallback_rates[] = { 44100, 48000, 32000, 22050, 11025 };

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
    opened_rate = sample_rate;

    mr = waveOutOpen(
            &ctx->hwo,
            WAVE_MAPPER,
            &wfx,
            (DWORD_PTR) dashcdg_winmm_callback,
            (DWORD_PTR) ctx,
            CALLBACK_FUNCTION
    );
    if (mr != MMSYSERR_NOERROR && audio->mode == DASHCDG_AUDIO_MODE_STREAM) {
        for (fallback_index = 0; fallback_index < (int) (sizeof(fallback_rates) / sizeof(fallback_rates[0])); ++fallback_index) {
            int alt = fallback_rates[fallback_index];

            if (alt <= 0 || alt == sample_rate) {
                continue;
            }
            wfx.nSamplesPerSec = (DWORD) alt;
            wfx.nAvgBytesPerSec = (DWORD) alt * (DWORD) wfx.nBlockAlign;
            mr = waveOutOpen(
                    &ctx->hwo,
                    WAVE_MAPPER,
                    &wfx,
                    (DWORD_PTR) dashcdg_winmm_callback,
                    (DWORD_PTR) ctx,
                    CALLBACK_FUNCTION
            );
            if (mr == MMSYSERR_NOERROR) {
                fprintf(
                        stderr,
                        "[desktop-audio] waveOutOpen @ %d Hz failed; retrying @ %d Hz\n",
                        sample_rate,
                        alt
                );
                fflush(stderr);
                opened_rate = alt;
                break;
            }
        }
    }
    if (mr != MMSYSERR_NOERROR) {
        (void) CloseHandle(ctx->done_evt);
        free(ctx->buffer_data);
        free(ctx);
        fprintf(stderr, "waveOutOpen failed: %u\n", (unsigned int) mr);
        return 0;
    }

    if (audio->mode == DASHCDG_AUDIO_MODE_STREAM && opened_rate != sample_rate) {
        ctx->sample_rate = (unsigned int) opened_rate;
        ctx->chunk_frames = ((unsigned long long) (unsigned int) opened_rate *
                (unsigned long long) DASHCDG_WINMM_STREAM_CHUNK_MS) / 1000ULL;
        if (ctx->chunk_frames < 1U) {
            ctx->chunk_frames = 1U;
        }
    }

    if (audio->mode == DASHCDG_AUDIO_MODE_STREAM) {
        pthread_mutex_lock(&audio->stream_mutex);
        audio->stream_sample_rate = (uint32_t) opened_rate;
        if ((uint32_t) opened_rate != audio->session_sample_rate) {
            fprintf(
                    stderr,
                    "[desktop-audio] waveOut device %d Hz vs session/decode %u Hz (resample in RX before queue; flushed pre-open ring).\n",
                    opened_rate,
                    (unsigned int) audio->session_sample_rate
            );
            fflush(stderr);
            dashcdg_desktop_audio_flush_stream_ring_locked(audio);
            DASHCDG_ATOMIC_SET(audio->timestamp_ms, -1);
        }
        pthread_mutex_unlock(&audio->stream_mutex);
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
#if defined(_WIN32)
    free(audio->stream_pa_i16_scratch);
#endif
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
    err = mp3dec_ex_open(&audio->stream_decoder, path, MP3D_DO_NOT_SCAN | MP3D_SEEK_TO_SAMPLE);
    if (err < 0) {
        fprintf(stderr, "failed to open streaming MP3 decoder: %d\n", err);
        memset(&audio->stream_decoder, 0, sizeof(audio->stream_decoder));
        return 0;
    }

    audio->stream_decoder_open = 1;
    return 1;
}

int dashcdg_desktop_audio_seek_mp3_stream(struct dashcdg_desktop_audio *audio, uint32_t seek_ms) {
    uint64_t pcm_pos;
    int channels;
    uint32_t hz;

    if (audio == NULL || !audio->stream_decoder_open) {
        return 0;
    }
    if ((audio->stream_decoder.flags & MP3D_SEEK_TO_SAMPLE) == 0) {
        return 0;
    }
    channels = audio->stream_decoder.info.channels;
    hz = (uint32_t) audio->stream_decoder.info.hz;
    if (channels <= 0 || hz == 0) {
        return 0;
    }
    /* minimp3 sample index is total interleaved PCM samples (all channels). */
    pcm_pos = (uint64_t) seek_ms * (uint64_t) hz / 1000ULL;
    pcm_pos *= (uint64_t) channels;
    if (mp3dec_ex_seek(&audio->stream_decoder, pcm_pos) != 0) {
        return 0;
    }
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

    /*
     * minimp3 uses int16 pcm unless built with MINIMP3_FLOAT_OUTPUT; desktop builds omit that,
     * so pcm is native int16 interleaved (matches TX fifo + Opus path).
     */
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
    uint32_t ring_sr;

    if (audio == NULL || sample_rate == 0 || channels == 0 || buffer_ms == 0) {
        return 0;
    }

    /*
     * Size the ring and request Pa_OpenStream at the session/decode rate. Using the host default
     * (often 44100) here forced Pa_OpenStream(44100) while the wire/session stayed 48000 — RX then
     * SRC'd every frame and, worse, pre-open queues held session-rate PCM that the DAC played at
     * the wrong clock until the mismatch was visible (audible pitch/SRC artifacts).
     */
    ring_sr = sample_rate;

    capacity_frames = ((size_t) ring_sr * (size_t) buffer_ms) / 1000U;
    if (capacity_frames < (size_t) ring_sr / 10U) {
        capacity_frames = (size_t) ring_sr / 10U;
    }

    free(audio->stream_pcm);
#if defined(_WIN32)
    free(audio->stream_pa_i16_scratch);
    audio->stream_pa_i16_scratch = NULL;
    audio->stream_pa_i16_scratch_samples = 0U;
#endif
    audio->stream_pcm = (int16_t *) calloc(capacity_frames * (size_t) channels, sizeof(int16_t));
    if (audio->stream_pcm == NULL) {
        return 0;
    }
#if defined(_WIN32)
    {
        /* Max single PortAudio block is at most a few 100ms; 64k int16s ≈ 340ms @ 48kHz stereo. */
        size_t scratch = (size_t) capacity_frames * (size_t) channels;

        if (scratch < 65536U) {
            scratch = 65536U;
        }
        audio->stream_pa_i16_scratch = (int16_t *) malloc(scratch * sizeof(int16_t));
        if (audio->stream_pa_i16_scratch == NULL) {
            free(audio->stream_pcm);
            audio->stream_pcm = NULL;
            return 0;
        }
        audio->stream_pa_i16_scratch_samples = scratch;
    }
#endif

    audio->mode = DASHCDG_AUDIO_MODE_STREAM;
    audio->session_sample_rate = sample_rate;
    audio->stream_sample_rate = ring_sr;
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
#if DASHCDG_HAVE_PORTAUDIO
    if (audio->stream != NULL && Pa_IsStreamActive(audio->stream) == 1) {
        DASHCDG_ATOMIC_SET(audio->playback_running, 1);
        return 1;
    }
#endif
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

/*
 * True once the host output device is accepting callbacks (PortAudio stream open, or WinMM waveOut).
 * Until then, queued PCM is session-rate decode; comparing it to init_stream()'s host-default guess
 * for stream_sample_rate mis-triggers SRC (e.g. 48 kHz frames resampled toward 44.1 kHz, then opened at 48 kHz).
 */
int dashcdg_desktop_audio_output_device_ready(const struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return 0;
    }
#if DASHCDG_HAVE_PORTAUDIO
    if (audio->stream != NULL) {
        return 1;
    }
#endif
#if defined(DASHCDG_DESKTOP_WIN32_WAVEOUT) && (DASHCDG_DESKTOP_WIN32_WAVEOUT) && defined(_WIN32)
    if (audio->audio_io_ctx != NULL) {
        return 1;
    }
#endif
    return 0;
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
    /*
     * Re-anchor whenever the stream refills from empty, not just on the very first queue.
     * Otherwise an underrun or track handoff can keep the old base timestamp alive and make
     * later RX timing/graphics reason about stale DAC time until the device is reopened.
     */
    if (frame_count > 0 && audio->stream_queued_frames == 0) {
        audio->stream_base_timestamp_ms = first_frame_timestamp_ms;
        audio->stream_played_frames = 0;
        DASHCDG_ATOMIC_SET(audio->timestamp_ms, -1);
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
    uint32_t rate_for_ms;

    if (audio == NULL) {
        return 0;
    }

    pthread_mutex_lock((pthread_mutex_t *) &audio->stream_mutex);
    queued_frames = audio->stream_queued_frames;
    pthread_mutex_unlock((pthread_mutex_t *) &audio->stream_mutex);

    /*
     * Pre-device-open, the ring holds session-clock PCM (decode rate). Init uses the host default
     * rate only for sizing/opening Pa_OpenStream — do not interpret queued frame counts in that Hz.
     */
    if (audio->mode == DASHCDG_AUDIO_MODE_STREAM &&
            !dashcdg_desktop_audio_output_device_ready(audio) &&
            audio->stream_sample_rate == audio->session_sample_rate) {
        rate_for_ms = audio->session_sample_rate;
    } else {
        rate_for_ms = audio->stream_sample_rate;
    }
    if (rate_for_ms == 0U) {
        return 0;
    }

    return (uint32_t) ((queued_frames * 1000U) / rate_for_ms);
}

uint32_t dashcdg_desktop_audio_output_latency_ms(const struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return 0U;
    }

#if DASHCDG_HAVE_PORTAUDIO
    if (audio->stream != NULL) {
        const PaStreamInfo *psi = Pa_GetStreamInfo(audio->stream);

        if (psi != NULL && psi->outputLatency > 0.0) {
            double ms = psi->outputLatency * 1000.0;

            if (ms <= 0.0) {
                return 0U;
            }
            if (ms >= 1000.0) {
                return 1000U;
            }
            return (uint32_t) (ms + 0.5);
        }
    }
#endif

#if DASHCDG_DESKTOP_WIN32_WAVEOUT && defined(_WIN32)
    if (audio->audio_io_ctx != NULL) {
        return audio->mode == DASHCDG_AUDIO_MODE_STREAM ? 160U : 80U;
    }
#endif

    return 0U;
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

uint32_t dashcdg_desktop_audio_session_sample_rate(const struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return 0U;
    }
    if (audio->mode == DASHCDG_AUDIO_MODE_STREAM) {
        return audio->session_sample_rate;
    }
    return (uint32_t) audio->file_info.hz;
}

uint32_t dashcdg_desktop_audio_output_sample_rate(const struct dashcdg_desktop_audio *audio) {
    if (audio == NULL) {
        return 0U;
    }
    if (audio->mode == DASHCDG_AUDIO_MODE_STREAM) {
        if (!dashcdg_desktop_audio_output_device_ready(audio) &&
                audio->stream_sample_rate == audio->session_sample_rate) {
            return audio->session_sample_rate != 0U ? audio->session_sample_rate : audio->stream_sample_rate;
        }
        return audio->stream_sample_rate;
    }
    return (uint32_t) audio->file_info.hz;
}

void dashcdg_desktop_audio_refresh_stream_sample_rate(struct dashcdg_desktop_audio *audio) {
#if DASHCDG_HAVE_PORTAUDIO
    const PaStreamInfo *psi;

    if (audio == NULL || audio->mode != DASHCDG_AUDIO_MODE_STREAM || audio->stream == NULL) {
        return;
    }
    psi = Pa_GetStreamInfo(audio->stream);
    if (psi != NULL) {
        audio->stream_sample_rate = (uint32_t) (psi->sampleRate + 0.5);
    }
#else
    (void) audio;
#endif
}
