/*
 * ffwrap — the C behind ../include/ffwrap.h: libavformat to read a file, libavcodec to decode its
 * video, libswscale to turn each frame into RGBA8 at the size asked for.
 *
 * Everything that knows FFmpeg's structs is in this file. The Java side sees a handle and numbers.
 */
#include "ffwrap.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>

#if defined(_MSC_VER)
#define FFW_TLS __declspec(thread)
#else
#define FFW_TLS _Thread_local
#endif

static FFW_TLS char err_text[512];
static FFW_TLS int err_set;

static void fail(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(err_text, sizeof err_text, fmt, args);
    va_end(args);
    err_set = 1;
}

/* FFmpeg's log, kept rather than printed: a library writing to the host's stderr is a library talking
 * over its caller. The last error a demuxer or decoder logged on this thread is often the better reason
 * ("moov atom not found" over "Invalid data found when processing input"), so fail_av adds it. */
static FFW_TLS char log_text[256];

static void keep_log(void *avcl, int level, const char *fmt, va_list args) {
    (void) avcl;
    if (level > AV_LOG_ERROR) {
        return;
    }
    vsnprintf(log_text, sizeof log_text, fmt, args);
    size_t n = strlen(log_text);
    while (n > 0 && (log_text[n - 1] == '\n' || log_text[n - 1] == '\r')) {
        log_text[--n] = '\0';
    }
}

/* What went wrong, in FFmpeg's words, after what this was doing. */
static void fail_av(const char *doing, int code) {
    char why[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, why, sizeof why);
    if (log_text[0] != '\0') {
        fail("%s: %s (%s)", doing, why, log_text);
    } else {
        fail("%s: %s", doing, why);
    }
}

struct FfwVideo {
    AVFormatContext *format;
    AVCodecContext *codec;
    AVStream *stream;
    int index;
    AVPacket *packet;
    AVFrame *frame;

    uint32_t width, height;         /* shown size: coded size, sample aspect applied */
    uint32_t out_w, out_h;          /* what each frame is scaled to */
    struct SwsContext *scale;
    uint8_t *pixels;                /* out_w * out_h * 4 */

    int64_t start;                  /* the stream's first timestamp, in its time base */
    int64_t pts_us;                 /* the frame last produced */
    int64_t skip_until_us;          /* after a seek, frames that end before this are decoded and dropped */
    int drained;                    /* the demuxer is spent and the decoder has been told so */
};

const char *ffw_version(void) {
    return av_version_info();
}

void ffw_close(FfwVideo *v) {
    if (v == NULL) {
        return;
    }
    sws_freeContext(v->scale);
    av_free(v->pixels);
    av_frame_free(&v->frame);
    av_packet_free(&v->packet);
    avcodec_free_context(&v->codec);
    avformat_close_input(&v->format);
    av_free(v);
}

/* (Re)size the output buffer. The scaler is made per frame, lazily, since the source format is only
 * known once a frame has been decoded, and sws_getCachedContext keeps it while nothing changes. */
static int resize(FfwVideo *v, uint32_t w, uint32_t h) {
    uint8_t *pixels = av_malloc((size_t) w * h * 4);
    if (pixels == NULL) {
        fail("out of memory for a %ux%u frame", w, h);
        return 0;
    }
    av_free(v->pixels);
    v->pixels = pixels;
    v->out_w = w;
    v->out_h = h;
    return 1;
}

FfwVideo *ffw_open(const char *path) {
    if (path == NULL || path[0] == '\0') {
        fail("no path to open");
        return NULL;
    }
    av_log_set_callback(keep_log);
    log_text[0] = '\0';
    FfwVideo *v = av_mallocz(sizeof *v);
    if (v == NULL) {
        fail("out of memory");
        return NULL;
    }
    int code = avformat_open_input(&v->format, path, NULL, NULL);
    if (code < 0) {
        fail_av("could not open the file", code);
        av_free(v);
        return NULL;
    }
    code = avformat_find_stream_info(v->format, NULL);
    if (code < 0) {
        fail_av("could not read the streams", code);
        ffw_close(v);
        return NULL;
    }
    const AVCodec *decoder = NULL;
    v->index = av_find_best_stream(v->format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (v->index < 0) {
        fail(v->index == AVERROR_DECODER_NOT_FOUND ? "no decoder for the video stream" : "no video stream");
        ffw_close(v);
        return NULL;
    }
    v->stream = v->format->streams[v->index];
    /* A cover picture is a stream of one frame; a file that is only that is not a video. */
    if (v->stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
        fail("no video stream, only a cover picture");
        ffw_close(v);
        return NULL;
    }
    v->codec = avcodec_alloc_context3(decoder);
    if (v->codec == NULL) {
        fail("out of memory for the decoder");
        ffw_close(v);
        return NULL;
    }
    code = avcodec_parameters_to_context(v->codec, v->stream->codecpar);
    if (code < 0) {
        fail_av("could not set the decoder up", code);
        ffw_close(v);
        return NULL;
    }
    v->codec->thread_count = 0;     /* as many as the machine has */
    v->codec->pkt_timebase = v->stream->time_base;
    code = avcodec_open2(v->codec, decoder, NULL);
    if (code < 0) {
        fail_av("could not open the decoder", code);
        ffw_close(v);
        return NULL;
    }
    v->packet = av_packet_alloc();
    v->frame = av_frame_alloc();
    if (v->packet == NULL || v->frame == NULL) {
        fail("out of memory");
        ffw_close(v);
        return NULL;
    }
    int w = v->codec->width;
    int h = v->codec->height;
    if (w <= 0 || h <= 0) {
        fail("the video stream has no size (%dx%d)", w, h);
        ffw_close(v);
        return NULL;
    }
    AVRational sar = av_guess_sample_aspect_ratio(v->format, v->stream, NULL);
    if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) {
        w = (int) av_rescale(w, sar.num, sar.den);
    }
    v->width = (uint32_t) (w > 0 ? w : 1);
    v->height = (uint32_t) h;
    v->start = v->stream->start_time != AV_NOPTS_VALUE ? v->stream->start_time : 0;
    if (!resize(v, v->width, v->height)) {
        ffw_close(v);
        return NULL;
    }
    return v;
}

uint32_t ffw_width(const FfwVideo *v) {
    return v->width;
}

uint32_t ffw_height(const FfwVideo *v) {
    return v->height;
}

int64_t ffw_duration_us(const FfwVideo *v) {
    if (v->format->duration != AV_NOPTS_VALUE && v->format->duration > 0) {
        return av_rescale_q(v->format->duration, AV_TIME_BASE_Q, (AVRational) {1, 1000000});
    }
    if (v->stream->duration != AV_NOPTS_VALUE && v->stream->duration > 0) {
        return av_rescale_q(v->stream->duration, v->stream->time_base, (AVRational) {1, 1000000});
    }
    return -1;
}

double ffw_frame_rate(const FfwVideo *v) {
    AVRational r = v->stream->avg_frame_rate;
    if (r.num <= 0 || r.den <= 0) {
        r = v->stream->r_frame_rate;
    }
    return r.num > 0 && r.den > 0 ? av_q2d(r) : 0.0;
}

const char *ffw_codec(const FfwVideo *v) {
    return avcodec_get_name(v->codec->codec_id);
}

const char *ffw_container(const FfwVideo *v) {
    return v->format->iformat->name;
}

int32_t ffw_has_audio(const FfwVideo *v) {
    for (unsigned i = 0; i < v->format->nb_streams; i++) {
        if (v->format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            return 1;
        }
    }
    return 0;
}

int32_t ffw_output(FfwVideo *v, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        fail("an output of %ux%u was asked for", width, height);
        return 0;
    }
    if (width == v->out_w && height == v->out_h) {
        return 1;
    }
    return resize(v, width, height);
}

/* The next decoded frame into v->frame: 1, or 0 at the end, or -1. Feeds the decoder packets of the
 * chosen stream until it gives one back, and tells it the end once the file has none left. */
static int decode(FfwVideo *v) {
    for (;;) {
        int code = avcodec_receive_frame(v->codec, v->frame);
        if (code == 0) {
            return 1;
        }
        if (code == AVERROR_EOF) {
            return 0;
        }
        if (code != AVERROR(EAGAIN)) {
            fail_av("could not decode a frame", code);
            return -1;
        }
        if (v->drained) {
            return 0;
        }
        code = av_read_frame(v->format, v->packet);
        if (code == AVERROR_EOF) {
            v->drained = 1;
            code = avcodec_send_packet(v->codec, NULL);
            if (code < 0 && code != AVERROR_EOF) {
                fail_av("could not finish decoding", code);
                return -1;
            }
            continue;
        }
        if (code < 0) {
            fail_av("could not read the file", code);
            return -1;
        }
        if (v->packet->stream_index == v->index) {
            code = avcodec_send_packet(v->codec, v->packet);
            av_packet_unref(v->packet);
            /* A damaged packet is the decoder's to skip, as a player would; only running out of memory stops. */
            if (code == AVERROR(ENOMEM)) {
                fail_av("could not decode a packet", code);
                return -1;
            }
        } else {
            av_packet_unref(v->packet);
        }
    }
}

static int64_t to_us(const FfwVideo *v, int64_t ts) {
    return av_rescale_q(ts - v->start, v->stream->time_base, (AVRational) {1, 1000000});
}

/* The YUV matrix the frame was made with. Unspecified is what most files say, and then the size is the
 * convention: high definition is BT.709, anything smaller BT.601. */
static const int *coefficients(const AVFrame *f) {
    int space = f->colorspace;
    if (space == AVCOL_SPC_UNSPECIFIED || space == AVCOL_SPC_RESERVED) {
        space = f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
    return sws_getCoefficients(space);
}

static int convert(FfwVideo *v) {
    AVFrame *f = v->frame;
    int flags = (uint32_t) f->width > v->out_w || (uint32_t) f->height > v->out_h ? SWS_AREA : SWS_BICUBIC;
    v->scale = sws_getCachedContext(v->scale, f->width, f->height, (enum AVPixelFormat) f->format,
                                    (int) v->out_w, (int) v->out_h, AV_PIX_FMT_RGBA,
                                    flags | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, NULL, NULL, NULL);
    if (v->scale == NULL) {
        fail("no conversion from %s at %dx%d to RGBA at %ux%u",
             av_get_pix_fmt_name((enum AVPixelFormat) f->format), f->width, f->height, v->out_w, v->out_h);
        return 0;
    }
    const int *matrix = coefficients(f);
    int full = f->color_range == AVCOL_RANGE_JPEG;
    /* Fails harmlessly for a source that is already RGB, where there is no matrix to set. */
    sws_setColorspaceDetails(v->scale, matrix, full, sws_getCoefficients(SWS_CS_DEFAULT), 1,
                             0, 1 << 16, 1 << 16);
    uint8_t *dst[4] = {v->pixels, NULL, NULL, NULL};
    int stride[4] = {(int) v->out_w * 4, 0, 0, 0};
    int rows = sws_scale(v->scale, (const uint8_t *const *) f->data, f->linesize, 0, f->height, dst, stride);
    if (rows <= 0) {
        fail("could not convert a frame to RGBA");
        return 0;
    }
    return 1;
}

int32_t ffw_next(FfwVideo *v) {
    for (;;) {
        int got = decode(v);
        if (got <= 0) {
            return got;
        }
        int64_t ts = v->frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) {
            ts = v->frame->pts;
        }
        int64_t us = ts == AV_NOPTS_VALUE ? v->pts_us : to_us(v, ts);
        if (v->skip_until_us > 0) {
            int64_t length = v->frame->duration > 0 ? to_us(v, v->start + v->frame->duration) : 0;
            if (us + length <= v->skip_until_us && !(length == 0 && us >= v->skip_until_us)) {
                av_frame_unref(v->frame);
                continue;
            }
            v->skip_until_us = 0;
        }
        v->pts_us = us;
        int ok = convert(v);
        av_frame_unref(v->frame);
        return ok ? 1 : -1;
    }
}

int64_t ffw_pts_us(const FfwVideo *v) {
    return v->pts_us;
}

const uint8_t *ffw_pixels(const FfwVideo *v) {
    return v->pixels;
}

int32_t ffw_seek(FfwVideo *v, int64_t us) {
    if (us < 0) {
        us = 0;
    }
    int64_t ts = v->start + av_rescale_q(us, (AVRational) {1, 1000000}, v->stream->time_base);
    int code = av_seek_frame(v->format, v->index, ts, AVSEEK_FLAG_BACKWARD);
    if (code < 0) {
        /* Some containers seek only by bytes or only to the start; the start always works. */
        code = avformat_seek_file(v->format, v->index, INT64_MIN, ts, ts, 0);
        if (code < 0) {
            fail_av("could not seek", code);
            return 0;
        }
    }
    avcodec_flush_buffers(v->codec);
    v->drained = 0;
    v->skip_until_us = us;
    v->pts_us = us;
    return 1;
}

const char *ffw_last_error(void) {
    return err_set ? err_text : NULL;
}
