/*
 * audio.c — the sound half of ../include/ffwrap.h: libavformat to read the file, libavcodec to decode
 * its sound, libswresample to make that the output's float stereo, and miniaudio to play it.
 *
 * Three threads touch an FfwAudio: the caller's, which steers; the decoder's, which keeps a ring of
 * samples filled a little ahead; and the device's, which drains the ring from its callback. The ring, the
 * clock's counters and the requests between them are guarded by one mutex, held only to copy samples or
 * set a field, never across a decode or a device call.
 *
 * The clock counts what the device has taken since the last seek, at the output's rate, less what the
 * device holds and has not played yet, and moves smoothly between its callbacks. Silence written into a
 * gap in the stream is counted like sound, so the count stays the stream's time.
 *
 * Every sample goes through level.c on its way into the ring, which measures it with the decoded sound after
 * it in view; the device turns those measures into a gain by the compression knob as it plays the sample. The
 * leveller holds samples back from the ring until it has seen far enough ahead, but they keep their places in
 * the stream, so the clock is as it would be without it.
 */
#include "ffwrap.h"
#include "internal.h"
#include "level.h"
#include "ma.h"

#include <math.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
typedef HANDLE ffwi_thread;
#else
#include <pthread.h>
typedef pthread_t ffwi_thread;
#endif

/* What the output is given: miniaudio maps two channels onto whatever the device has. */
#define CHANNELS 2

struct FfwAudio {
    /* The decoder's: touched by its thread alone once that runs, and by open and close around it. */
    AVFormatContext *format;
    AVCodecContext *codec;
    AVStream *stream;
    int index;
    AVPacket *packet;
    AVFrame *frame;
    struct SwrContext *convert;     /* made for the first frame, and again whenever the input changes */
    int in_rate, in_format;
    AVChannelLayout in_layout;
    float *chunk;                   /* one frame, converted */
    int chunk_cap;                  /* in sample frames */
    int64_t start;                  /* the stream's first timestamp, in its time base */
    int drained;                    /* the demuxer is spent and the decoder has been told so */
    FfwLevel *level;                /* every sample's gain, from what is around it */

    ma_context context;
    ma_device device;
    uint32_t rate;                  /* the output's samples per second */
    int64_t period_us;              /* how often the device takes samples */
    int64_t latency_us;             /* how long a sample it has taken waits to be heard, at most */

    ffwi_thread thread;
    ma_mutex lock;
    ma_event wake;                  /* to the decoder: room in the ring, a seek, or close */
    int context_ready, device_ready, sync_ready, thread_ready;

    /* Guarded by lock. */
    float *ring;                    /* cap sample frames of CHANNELS floats, held of them from head on */
    float *measures;                /* each of those frames' FFWI_LEVEL_MEASURES measures from the leveller */
    uint32_t cap, head, held;
    FfwLevelKnob knob;              /* the compression, as the settings it stands for */
    FfwLevelHeld released;          /* the gains just played, which the next come back up from */
    float *heard;                   /* the last heard_cap sample frames given to the device, ending at heard_at */
    uint32_t heard_cap, heard_at, heard_held;
    int64_t base_us;                /* the stream time of the first sample after the last seek */
    uint64_t played;                /* sample frames the device has taken since then */
    int64_t tick_us;                /* when it last took some, on av_gettime_relative's clock */
    int64_t last_us;                /* the clock as last told, from base_us: it never goes back */
    int64_t seek_to;                /* a seek the decoder has yet to make, or -1 */
    int finished;                   /* the decoder has written its last sample */
    int playing;
    int closing;
};

/* --- the clock ----------------------------------------------------------------------------------- */

/* The stream time being heard now, less base_us. Under lock. */
static int64_t heard_locked(FfwAudio *a) {
    int64_t taken = av_rescale((int64_t) a->played, 1000000, a->rate);
    int64_t t = taken;
    if (a->playing) {
        /* Taken is not yet heard: the device plays it out over its latency. Between its callbacks the clock
         * runs on by the wall clock, up to a period; after the last sample, up to the whole latency. */
        int64_t since = a->played > 0 ? av_gettime_relative() - a->tick_us : 0;
        int64_t most = a->finished && a->held == 0 ? a->latency_us + a->period_us : a->period_us;
        t = taken - a->latency_us + (since < 0 ? 0 : since < most ? since : most);
        if (t > taken) {
            t = taken;
        }
    }
    /* Paused, the device has played out what it took (stopping drains it), so taken is heard. A clock
     * never goes back: not when a device's periods come unevenly, nor while what is taken after a resume
     * waits out the latency. */
    if (t < a->last_us) {
        t = a->last_us;
    }
    a->last_us = t;
    return t;
}

/* --- the device ---------------------------------------------------------------------------------- */

static void on_device(ma_device *device, void *output, const void *input, ma_uint32 count) {
    (void) input;
    FfwAudio *a = device->pUserData;
    float *out = output;            /* already silence: miniaudio clears it, so an underrun is quiet */
    int took = 0;
    ma_mutex_lock(&a->lock);
    if (a->held > 0) {
        uint32_t k = count < a->held ? count : a->held;
        for (uint32_t i = 0; i < k; i++) {
            uint32_t at = (a->head + i) % a->cap;
            float m = 1.0f;
            if (a->knob.on) {
                float db = ffwi_level_gain(&a->knob, a->measures + (size_t) at * FFWI_LEVEL_MEASURES, &a->released);
                m = powf(10.0f, db / 20.0f);
            }
            float *o = out + (size_t) i * CHANNELS;
            for (int ch = 0; ch < CHANNELS; ch++) {
                o[ch] = a->ring[(size_t) at * CHANNELS + ch] * m;
            }
            memcpy(a->heard + (size_t) a->heard_at * CHANNELS, o, CHANNELS * sizeof(float));
            a->heard_at = (a->heard_at + 1) % a->heard_cap;
        }
        a->heard_held = a->heard_held + k < a->heard_cap ? a->heard_held + k : a->heard_cap;
        a->head = (a->head + k) % a->cap;
        a->held -= k;
        a->played += k;
        a->tick_us = av_gettime_relative();
        took = 1;
    }
    ma_mutex_unlock(&a->lock);
    if (took) {
        ma_event_signal(&a->wake);
    }
}

/* --- the decoder --------------------------------------------------------------------------------- */

/* Write n sample frames and their measures to the ring, waiting for room. 0 if a seek or close came first,
 * and what was left is dropped. The leveller's sink. */
static int put(void *to, const float *src, const float *measures, int64_t n) {
    FfwAudio *a = to;
    while (n > 0) {
        ma_mutex_lock(&a->lock);
        if (a->closing || a->seek_to >= 0) {
            ma_mutex_unlock(&a->lock);
            return 0;
        }
        uint32_t room = a->cap - a->held;
        if (room == 0) {
            ma_mutex_unlock(&a->lock);
            ma_event_wait(&a->wake);
            continue;
        }
        uint32_t k = n < room ? (uint32_t) n : room;
        uint32_t tail = (a->head + a->held) % a->cap;
        uint32_t first = k < a->cap - tail ? k : a->cap - tail;
        float *at = a->ring + (size_t) tail * CHANNELS;
        size_t one = (size_t) first * CHANNELS * sizeof(float);
        size_t two = (size_t) (k - first) * CHANNELS * sizeof(float);
        memcpy(at, src, one);
        memcpy(a->ring, src + (size_t) first * CHANNELS, two);
        memcpy(a->measures + (size_t) tail * FFWI_LEVEL_MEASURES, measures,
               (size_t) first * FFWI_LEVEL_MEASURES * sizeof(float));
        memcpy(a->measures, measures + (size_t) first * FFWI_LEVEL_MEASURES,
               (size_t) (k - first) * FFWI_LEVEL_MEASURES * sizeof(float));
        src += (size_t) k * CHANNELS;
        measures += (size_t) k * FFWI_LEVEL_MEASURES;
        a->held += k;
        ma_mutex_unlock(&a->lock);
        n -= k;
    }
    return 1;
}

/* The next decoded frame into a->frame: 1, or 0 at the end, or -1 if the file cannot be read on. As
 * ffwrap.c's decode, for the sound's stream. */
static int decode(FfwAudio *a) {
    for (;;) {
        int code = avcodec_receive_frame(a->codec, a->frame);
        if (code == 0) {
            return 1;
        }
        if (code == AVERROR_EOF) {
            return 0;
        }
        if (code != AVERROR(EAGAIN)) {
            return -1;
        }
        if (a->drained) {
            return 0;
        }
        code = av_read_frame(a->format, a->packet);
        if (code == AVERROR_EOF) {
            a->drained = 1;
            avcodec_send_packet(a->codec, NULL);
            continue;
        }
        if (code < 0) {
            return -1;
        }
        if (a->packet->stream_index == a->index) {
            code = avcodec_send_packet(a->codec, a->packet);
            av_packet_unref(a->packet);
            /* A damaged packet is the decoder's to skip, as a player would. */
            if (code == AVERROR(ENOMEM)) {
                return -1;
            }
        } else {
            av_packet_unref(a->packet);
        }
    }
}

/* A converter from this frame's samples to the output's, kept while frames keep coming the same way. */
static int converter(FfwAudio *a, const AVFrame *f) {
    if (a->convert != NULL && f->sample_rate == a->in_rate && f->format == a->in_format
            && av_channel_layout_compare(&f->ch_layout, &a->in_layout) == 0) {
        return 1;
    }
    swr_free(&a->convert);
    av_channel_layout_uninit(&a->in_layout);
    if (av_channel_layout_copy(&a->in_layout, &f->ch_layout) < 0) {
        return 0;
    }
    AVChannelLayout in;
    if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_default(&in, f->ch_layout.nb_channels);   /* a count alone: the usual layout for it */
    } else if (av_channel_layout_copy(&in, &f->ch_layout) < 0) {
        return 0;
    }
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    int code = swr_alloc_set_opts2(&a->convert, &stereo, AV_SAMPLE_FMT_FLT, (int) a->rate,
                                   &in, (enum AVSampleFormat) f->format, f->sample_rate, 0, NULL);
    av_channel_layout_uninit(&in);
    if (code < 0 || swr_init(a->convert) < 0) {
        swr_free(&a->convert);
        return 0;
    }
    a->in_rate = f->sample_rate;
    a->in_format = f->format;
    return 1;
}

/* Convert a->frame and write it where it falls: what a seek overshot is cut off the front, a gap before
 * it is filled with silence, and an overlap with what is written already is cut. *pos counts the sample
 * frames written since base_us. 0 if a seek or close came first. */
static int place(FfwAudio *a, int64_t base_us, int64_t *pos) {
    AVFrame *f = a->frame;
    if (!converter(a, f)) {
        return 1;                   /* a frame nothing converts is skipped, as a damaged one is */
    }
    int room = swr_get_out_samples(a->convert, f->nb_samples);
    if (room > a->chunk_cap) {
        float *chunk = av_realloc(a->chunk, (size_t) room * CHANNELS * sizeof(float));
        if (chunk == NULL) {
            return 1;
        }
        a->chunk = chunk;
        a->chunk_cap = room;
    }
    uint8_t *out[1] = {(uint8_t *) a->chunk};
    int n = swr_convert(a->convert, out, room, (const uint8_t **) f->extended_data, f->nb_samples);
    if (n <= 0) {
        return 1;
    }
    int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    int64_t at = *pos;
    if (ts != AV_NOPTS_VALUE) {
        int64_t us = av_rescale_q(ts - a->start, a->stream->time_base, (AVRational) {1, 1000000});
        at = av_rescale(us - base_us, a->rate, 1000000);
    }
    /* Exact at a seek's first sample; after it, timestamps that wander by less than 10 ms are rounding,
     * not gaps, and the sound runs on as it was written. */
    int64_t slack = *pos == 0 ? 0 : a->rate / 100;
    int64_t off = at - *pos;
    const float *src = a->chunk;
    if (off > slack) {
        if (!ffwi_level_push(a->level, NULL, off, put, a)) {
            return 0;
        }
        *pos += off;
    } else if (off < -slack) {
        if (-off >= n) {
            return 1;
        }
        src += (size_t) -off * CHANNELS;
        n -= (int) -off;
    }
    if (!ffwi_level_push(a->level, src, n, put, a)) {
        return 0;
    }
    *pos += n;
    return 1;
}

/* Go to `us` in the file: the keyframe at or before it, the decoder and converter emptied. The samples
 * before `us` are cut by place. If the file will not seek, decoding goes on from where it was, and place
 * still puts what comes at its time. */
static void relocate(FfwAudio *a, int64_t us) {
    int64_t ts = a->start + av_rescale_q(us, (AVRational) {1, 1000000}, a->stream->time_base);
    if (av_seek_frame(a->format, a->index, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        avformat_seek_file(a->format, a->index, INT64_MIN, ts, ts, 0);
    }
    avcodec_flush_buffers(a->codec);
    swr_free(&a->convert);
    a->drained = 0;
}

static void run(FfwAudio *a) {
    int64_t base_us = 0;
    int64_t pos = 0;
    for (;;) {
        ma_mutex_lock(&a->lock);
        while (!a->closing && a->seek_to < 0 && a->finished) {
            ma_mutex_unlock(&a->lock);
            ma_event_wait(&a->wake);
            ma_mutex_lock(&a->lock);
        }
        if (a->closing) {
            ma_mutex_unlock(&a->lock);
            return;
        }
        int64_t target = a->seek_to;
        a->seek_to = -1;
        ma_mutex_unlock(&a->lock);

        if (target >= 0) {
            relocate(a, target);
            ffwi_level_reset(a->level);
            base_us = target;
            pos = 0;
            continue;
        }
        int got = decode(a);
        if (got > 0) {
            place(a, base_us, &pos);
            av_frame_unref(a->frame);
            continue;
        }
        /* The end, or a file that cannot be read on: either way there is no more sound until a seek. What
         * the leveller holds goes to the ring first, looking ahead only as far as there is. */
        int all = ffwi_level_finish(a->level, put, a);
        ma_mutex_lock(&a->lock);
        if (all && a->seek_to < 0) {
            a->finished = 1;
        }
        ma_mutex_unlock(&a->lock);
    }
}

#ifdef _WIN32
static unsigned __stdcall run_thread(void *a) {
    run(a);
    return 0;
}
#else
static void *run_thread(void *a) {
    run(a);
    return NULL;
}
#endif

/* --- the handle ---------------------------------------------------------------------------------- */

void ffw_audio_close(FfwAudio *a) {
    if (a == NULL) {
        return;
    }
    if (a->device_ready) {
        ma_device_uninit(&a->device);       /* stops it: no callback runs after this */
    }
    if (a->thread_ready) {
        ma_mutex_lock(&a->lock);
        a->closing = 1;
        ma_mutex_unlock(&a->lock);
        ma_event_signal(&a->wake);
#ifdef _WIN32
        WaitForSingleObject(a->thread, INFINITE);
        CloseHandle(a->thread);
#else
        pthread_join(a->thread, NULL);
#endif
    }
    if (a->context_ready) {
        ma_context_uninit(&a->context);
    }
    if (a->sync_ready) {
        ma_event_uninit(&a->wake);
        ma_mutex_uninit(&a->lock);
    }
    av_free(a->ring);
    av_free(a->measures);
    av_free(a->heard);
    ffwi_level_free(a->level);
    av_free(a->chunk);
    swr_free(&a->convert);
    av_channel_layout_uninit(&a->in_layout);
    av_frame_free(&a->frame);
    av_packet_free(&a->packet);
    avcodec_free_context(&a->codec);
    avformat_close_input(&a->format);
    av_free(a);
}

/* The output, ready to start: the device, its rate and latency, and the ring sized for it. */
static int open_output(FfwAudio *a, int32_t output) {
    ma_backend nowhere[] = {ma_backend_null};
    ma_result r = output == FFW_AUDIO_NOWHERE ? ma_context_init(nowhere, 1, NULL, &a->context)
                                              : ma_context_init(NULL, 0, NULL, &a->context);
    if (r != MA_SUCCESS) {
        ffwi_fail("no sound output: %s", ma_result_description(r));
        return 0;
    }
    a->context_ready = 1;
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = CHANNELS;
    config.sampleRate = 0;              /* the device's own, so miniaudio need not resample again */
    config.dataCallback = on_device;
    config.pUserData = a;
    r = ma_device_init(&a->context, &config, &a->device);
    if (r != MA_SUCCESS) {
        ffwi_fail("could not open the sound output: %s", ma_result_description(r));
        return 0;
    }
    a->device_ready = 1;
    a->rate = a->device.sampleRate;
    uint32_t inner = a->device.playback.internalSampleRate ? a->device.playback.internalSampleRate : a->rate;
    a->period_us = av_rescale(a->device.playback.internalPeriodSizeInFrames, 1000000, inner);
    a->latency_us = a->period_us * (a->device.playback.internalPeriods ? a->device.playback.internalPeriods : 1);
    a->cap = a->rate / 2;               /* half a second ahead: enough to ride out a slow decode */
    a->ring = av_malloc((size_t) a->cap * CHANNELS * sizeof(float));
    a->measures = av_malloc((size_t) a->cap * FFWI_LEVEL_MEASURES * sizeof(float));
    ffwi_level_knob(&a->knob, 0.0f, a->rate);
    a->heard_cap = a->rate / 4;         /* a quarter of a second of what was played, for a meter or a scope */
    a->heard = av_malloc((size_t) a->heard_cap * CHANNELS * sizeof(float));
    a->level = a->rate > 0 ? ffwi_level_new(a->rate) : NULL;
    if (a->rate == 0 || a->ring == NULL || a->measures == NULL || a->heard == NULL || a->level == NULL) {
        ffwi_fail(a->rate == 0 ? "the sound output has no rate" : "out of memory");
        return 0;
    }
    return 1;
}

FfwAudio *ffw_audio_open(const char *path, int32_t output) {
    if (path == NULL || path[0] == '\0') {
        ffwi_fail("no path to open");
        return NULL;
    }
    if (output != FFW_AUDIO_SPEAKERS && output != FFW_AUDIO_NOWHERE) {
        ffwi_fail("no output %d", output);
        return NULL;
    }
    ffwi_listen();
    FfwAudio *a = av_mallocz(sizeof *a);
    if (a == NULL) {
        ffwi_fail("out of memory");
        return NULL;
    }
    a->seek_to = -1;
    int code = avformat_open_input(&a->format, path, NULL, NULL);
    if (code < 0) {
        ffwi_fail_av("could not open the file", code);
        av_free(a);
        return NULL;
    }
    code = avformat_find_stream_info(a->format, NULL);
    if (code < 0) {
        ffwi_fail_av("could not read the streams", code);
        ffw_audio_close(a);
        return NULL;
    }
    const AVCodec *decoder = NULL;
    int video = av_find_best_stream(a->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    a->index = av_find_best_stream(a->format, AVMEDIA_TYPE_AUDIO, -1, video >= 0 ? video : -1, &decoder, 0);
    if (a->index < 0) {
        ffwi_fail(a->index == AVERROR_DECODER_NOT_FOUND ? "no decoder for the sound" : "no sound");
        ffw_audio_close(a);
        return NULL;
    }
    a->stream = a->format->streams[a->index];
    for (unsigned i = 0; i < a->format->nb_streams; i++) {
        if ((int) i != a->index) {
            a->format->streams[i]->discard = AVDISCARD_ALL;     /* the pictures are another handle's */
        }
    }
    a->codec = avcodec_alloc_context3(decoder);
    if (a->codec == NULL) {
        ffwi_fail("out of memory for the decoder");
        ffw_audio_close(a);
        return NULL;
    }
    code = avcodec_parameters_to_context(a->codec, a->stream->codecpar);
    if (code < 0) {
        ffwi_fail_av("could not set the decoder up", code);
        ffw_audio_close(a);
        return NULL;
    }
    a->codec->pkt_timebase = a->stream->time_base;
    code = avcodec_open2(a->codec, decoder, NULL);
    if (code < 0) {
        ffwi_fail_av("could not open the decoder", code);
        ffw_audio_close(a);
        return NULL;
    }
    a->packet = av_packet_alloc();
    a->frame = av_frame_alloc();
    if (a->packet == NULL || a->frame == NULL) {
        ffwi_fail("out of memory");
        ffw_audio_close(a);
        return NULL;
    }
    a->start = a->stream->start_time != AV_NOPTS_VALUE ? a->stream->start_time : 0;

    if (ma_mutex_init(&a->lock) != MA_SUCCESS) {
        ffwi_fail("could not make a lock");
        ffw_audio_close(a);
        return NULL;
    }
    if (ma_event_init(&a->wake) != MA_SUCCESS) {
        ma_mutex_uninit(&a->lock);
        ffwi_fail("could not make an event");
        ffw_audio_close(a);
        return NULL;
    }
    a->sync_ready = 1;
    if (!open_output(a, output)) {
        ffw_audio_close(a);
        return NULL;
    }
#ifdef _WIN32
    uintptr_t thread = _beginthreadex(NULL, 0, run_thread, a, 0, NULL);
    a->thread_ready = thread != 0;
    a->thread = (HANDLE) thread;
#else
    a->thread_ready = pthread_create(&a->thread, NULL, run_thread, a) == 0;
#endif
    if (!a->thread_ready) {
        ffwi_fail("could not start the decoding thread");
        ffw_audio_close(a);
        return NULL;
    }
    return a;
}

const char *ffw_audio_codec(const FfwAudio *a) {
    return avcodec_get_name(a->codec->codec_id);
}

uint32_t ffw_audio_channels(const FfwAudio *a) {
    return (uint32_t) a->codec->ch_layout.nb_channels;
}

uint32_t ffw_audio_rate(const FfwAudio *a) {
    return (uint32_t) a->codec->sample_rate;
}

int32_t ffw_audio_play(FfwAudio *a) {
    if (ma_device_get_state(&a->device) == ma_device_state_started) {
        return 1;
    }
    ma_mutex_lock(&a->lock);
    a->playing = 1;
    a->tick_us = av_gettime_relative();
    ma_mutex_unlock(&a->lock);
    ma_result r = ma_device_start(&a->device);
    if (r != MA_SUCCESS) {
        ma_mutex_lock(&a->lock);
        a->playing = 0;
        ma_mutex_unlock(&a->lock);
        ffwi_fail("could not start the sound output: %s", ma_result_description(r));
        return 0;
    }
    return 1;
}

int32_t ffw_audio_pause(FfwAudio *a) {
    if (ma_device_get_state(&a->device) != ma_device_state_started) {
        return 1;
    }
    ma_result r = ma_device_stop(&a->device);
    if (r != MA_SUCCESS) {
        ffwi_fail("could not stop the sound output: %s", ma_result_description(r));
        return 0;
    }
    ma_mutex_lock(&a->lock);
    a->playing = 0;
    ma_mutex_unlock(&a->lock);
    return 1;
}

int32_t ffw_audio_seek(FfwAudio *a, int64_t us) {
    if (us < 0) {
        us = 0;
    }
    ma_mutex_lock(&a->lock);
    a->seek_to = us;
    memset(&a->released, 0, sizeof a->released);
    a->base_us = us;
    a->head = 0;
    a->held = 0;
    a->played = 0;
    a->last_us = 0;
    a->finished = 0;
    ma_mutex_unlock(&a->lock);
    ma_event_signal(&a->wake);
    return 1;
}

void ffw_audio_volume(FfwAudio *a, float volume) {
    ma_device_set_master_volume(&a->device, volume < 0.0f ? 0.0f : volume > 1.0f ? 1.0f : volume);
}

void ffw_audio_compression(FfwAudio *a, float amount) {
    ma_mutex_lock(&a->lock);
    int was = a->knob.on;
    ffwi_level_knob(&a->knob, amount, a->rate);
    if (!was) {
        memset(&a->released, 0, sizeof a->released);    /* turned on: nothing played with it to come up from */
    }
    ma_mutex_unlock(&a->lock);
}

uint32_t ffw_audio_heard(FfwAudio *a, float *dst, uint32_t frames) {
    ma_mutex_lock(&a->lock);
    uint32_t k = frames < a->heard_held ? frames : a->heard_held;
    uint32_t from = (a->heard_at + a->heard_cap - k) % a->heard_cap;
    uint32_t first = k < a->heard_cap - from ? k : a->heard_cap - from;
    memcpy(dst, a->heard + (size_t) from * CHANNELS, (size_t) first * CHANNELS * sizeof(float));
    memcpy(dst + (size_t) first * CHANNELS, a->heard, (size_t) (k - first) * CHANNELS * sizeof(float));
    ma_mutex_unlock(&a->lock);
    return k;
}

int64_t ffw_audio_clock_us(FfwAudio *a) {
    ma_mutex_lock(&a->lock);
    int64_t us = a->base_us + heard_locked(a);
    ma_mutex_unlock(&a->lock);
    return us;
}

int32_t ffw_audio_ended(FfwAudio *a) {
    ma_mutex_lock(&a->lock);
    int ended = a->finished && a->held == 0
            && heard_locked(a) >= av_rescale((int64_t) a->played, 1000000, a->rate);
    ma_mutex_unlock(&a->lock);
    return ended;
}
