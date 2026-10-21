/*
 * encode — the recording half of ../include/ffwrap.h: RGBA8 frames in, AV1 out, in whatever container the file's
 * name asks for (Matroska or WebM, which time every frame on its own). libswscale to YUV, libavcodec to encode,
 * libavformat to write.
 *
 * Real time first. The software encoder is SVT-AV1 in its real-time mode (no look-ahead, no reordering), which
 * keeps up with a 1080p window at 60 fps on well under a core of a laptop CPU; a GPU's AV1 encoder is tried first
 * only when asked for, and the software one follows it whenever none opens. Every one is LGPL-clean: SVT-AV1 is
 * BSD, the hardware ones are the vendors' drivers.
 */
#include "ffwrap.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

struct FfwWriter {
    AVFormatContext *format;
    AVStream *stream;
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packet;
    struct SwsContext *scale;       /* for frames of scale_w x scale_h in scale_from, as last handed in */
    uint32_t scale_w, scale_h;
    enum AVPixelFormat scale_from;
    uint32_t width, height;         /* the canvas: what every frame in the file is */
    int64_t last_ms;                /* the time of the last frame encoded, in ms; -1 before the first */
    int64_t frames;
    int header;                     /* the container's header is written, so a trailer is owed */
};

/* SVT-AV1 prints its configuration to stderr when it opens, unless told otherwise; FFmpeg's own log is kept
 * (ffwi_listen), and SVT's is quietened to its fatal errors the same way. Only where the host has not set it. */
static void quieten_svt(void) {
#ifdef _WIN32
    size_t set = 0;
    getenv_s(&set, NULL, 0, "SVT_LOG");
    if (set == 0) {
        _putenv_s("SVT_LOG", "0");
    }
#else
    setenv("SVT_LOG", "0", 0);
#endif
}

/* The encoder's own pixel format: planar 4:2:0 where it takes it, else NV12 (the hardware ones). */
static enum AVPixelFormat pixel_format(const AVCodec *c) {
    const void *list = NULL;
    int n = 0;
    if (avcodec_get_supported_config(NULL, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, &list, &n) < 0 || list == NULL) {
        return AV_PIX_FMT_YUV420P;
    }
    const enum AVPixelFormat *formats = list;
    enum AVPixelFormat found = AV_PIX_FMT_NONE;
    for (int i = 0; i < n; i++) {
        if (formats[i] == AV_PIX_FMT_YUV420P) {
            return AV_PIX_FMT_YUV420P;
        }
        if (formats[i] == AV_PIX_FMT_NV12) {
            found = AV_PIX_FMT_NV12;
        }
    }
    return found;
}

/* `quality` is AV1's quantiser scale, 0 (best) to 63 (smallest), as SVT-AV1's crf takes it; each hardware encoder
 * has its own scale, and gets the same place on it. */
static void tune(AVCodecContext *c, const char *name, int quality) {
    if (strcmp(name, "libsvtav1") == 0) {
        av_opt_set_int(c->priv_data, "preset", c->width * c->height > 2560 * 1440 ? 12 : 10, 0);
        av_opt_set_int(c->priv_data, "crf", quality, 0);
        av_opt_set(c->priv_data, "svtav1-params", "rtc=1", 0);
    } else if (strcmp(name, "av1_nvenc") == 0) {
        av_opt_set(c->priv_data, "preset", "p4", 0);
        av_opt_set(c->priv_data, "tune", "ll", 0);
        av_opt_set(c->priv_data, "rc", "vbr", 0);
        av_opt_set_int(c->priv_data, "cq", quality * 51 / 63, 0);
        c->bit_rate = 0;
    } else if (strcmp(name, "av1_qsv") == 0) {
        av_opt_set(c->priv_data, "preset", "veryfast", 0);
        av_opt_set_int(c->priv_data, "low_delay_brc", 1, 0);
        c->global_quality = quality * 51 / 63 > 0 ? quality * 51 / 63 : 1;     /* ICQ */
    } else if (strcmp(name, "av1_amf") == 0) {
        av_opt_set(c->priv_data, "usage", "lowlatency", 0);
        av_opt_set(c->priv_data, "rc", "cqp", 0);
        av_opt_set_int(c->priv_data, "qp_i", quality * 4, 0);
        av_opt_set_int(c->priv_data, "qp_p", quality * 4, 0);
    }
}

/* Open the encoder called `name` for w, or leave w->codec NULL and say why. */
static int open_encoder(FfwWriter *w, const char *name, double fps, int quality) {
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (codec == NULL) {
        ffwi_fail("no %s encoder in this FFmpeg", name);
        return 0;
    }
    enum AVPixelFormat format = pixel_format(codec);
    if (format == AV_PIX_FMT_NONE) {
        ffwi_fail("%s takes neither yuv420p nor nv12", name);
        return 0;
    }
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (c == NULL) {
        ffwi_fail("out of memory for the encoder");
        return 0;
    }
    c->width = (int) w->width;
    c->height = (int) w->height;
    c->pix_fmt = format;
    /* Milliseconds, which is what Matroska keeps; the rate is a hint for rate control, never a schedule. */
    c->time_base = (AVRational) {1, 1000};
    c->framerate = av_d2q(fps, 1000);
    c->gop_size = (int) (fps * 5 + 0.5);    /* a keyframe every five seconds: what an exact seek decodes from */
    c->max_b_frames = 0;
    /* What a window is: sRGB, kept at full range so no shade of the interface is squeezed into 16-235. */
    c->color_range = AVCOL_RANGE_JPEG;
    c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_IEC61966_2_1;
    c->thread_count = 0;
    if (w->format->oformat->flags & AVFMT_GLOBALHEADER) {
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    tune(c, name, quality);
    int code = avcodec_open2(c, codec, NULL);
    if (code < 0) {
        ffwi_fail_av(name, code);
        avcodec_free_context(&c);
        return 0;
    }
    w->codec = c;
    return 1;
}

/* The conversion to YUV for frames of width x height in `from` (RGBA or BGRA), made when either changes. It writes into
 * the top left of the canvas, at the frame's own size: nothing is scaled. 1, or 0 and why. */
static int converter(FfwWriter *w, uint32_t width, uint32_t height, enum AVPixelFormat from) {
    if (w->scale != NULL && width == w->scale_w && height == w->scale_h && from == w->scale_from) {
        return 1;
    }
    sws_freeContext(w->scale);
    w->scale = sws_getContext((int) width, (int) height, from, (int) width, (int) height,
                              w->codec->pix_fmt, SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP,
                              NULL, NULL, NULL);
    if (w->scale == NULL) {
        ffwi_fail("no conversion from %s to %s at %ux%u", av_get_pix_fmt_name(from),
                  av_get_pix_fmt_name(w->codec->pix_fmt), width, height);
        return 0;
    }
    const int *bt709 = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(w->scale, bt709, 1, bt709, 1, 0, 1 << 16, 1 << 16);
    w->scale_w = width;
    w->scale_h = height;
    w->scale_from = from;
    return 1;
}

static void writer_free(FfwWriter *w) {
    sws_freeContext(w->scale);
    av_frame_free(&w->frame);
    av_packet_free(&w->packet);
    avcodec_free_context(&w->codec);
    if (w->format != NULL) {
        avio_closep(&w->format->pb);
        avformat_free_context(w->format);
    }
    av_free(w);
}

FfwWriter *ffw_writer_open(const char *path, uint32_t width, uint32_t height, double fps, int32_t quality,
                           int32_t encoder) {
    if (path == NULL || path[0] == '\0') {
        ffwi_fail("no path to write");
        return NULL;
    }
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        ffwi_fail("a %ux%u recording was asked for", width, height);
        return NULL;
    }
    if (!(fps > 0 && fps <= 1000)) {
        ffwi_fail("a rate of %g frames a second was asked for", fps);
        return NULL;
    }
    if (quality < 0 || quality > 63) {
        ffwi_fail("a quality of %d was asked for, where 0 to 63 is the scale", quality);
        return NULL;
    }
    ffwi_listen();
    quieten_svt();
    FfwWriter *w = av_mallocz(sizeof *w);
    if (w == NULL) {
        ffwi_fail("out of memory");
        return NULL;
    }
    w->width = width;
    w->height = height;
    w->last_ms = -1;
    int code = avformat_alloc_output_context2(&w->format, NULL, NULL, path);
    if (code < 0 || w->format == NULL) {
        ffwi_fail("no container for %s: name it .mkv or .webm", path);
        writer_free(w);
        return NULL;
    }
    if (avformat_query_codec(w->format->oformat, AV_CODEC_ID_AV1, FF_COMPLIANCE_NORMAL) != 1) {
        ffwi_fail("%s cannot hold AV1: name it .mkv or .webm", w->format->oformat->name);
        writer_free(w);
        return NULL;
    }

    /* The GPU's encoders, each tried until one opens (a machine without that vendor's GPU, or with one too old for
     * AV1, refuses at open), then SVT-AV1, which always can. */
    static const char *const hardware[] = {"av1_nvenc", "av1_qsv", "av1_amf"};
    char tried[256] = "";
    if (encoder == FFW_ENCODE_HARDWARE) {
        for (size_t i = 0; i < sizeof hardware / sizeof *hardware && w->codec == NULL; i++) {
            if (!open_encoder(w, hardware[i], fps, quality)) {
                size_t n = strlen(tried);
                snprintf(tried + n, sizeof tried - n, "%s; ", ffw_last_error());
            }
        }
    }
    if (w->codec == NULL && !open_encoder(w, "libsvtav1", fps, quality)) {
        if (tried[0] != '\0') {
            char last[512];
            snprintf(last, sizeof last, "%s", ffw_last_error());
            ffwi_fail("no AV1 encoder opened: %s%s", tried, last);
        }
        writer_free(w);
        return NULL;
    }

    w->stream = avformat_new_stream(w->format, NULL);
    w->frame = av_frame_alloc();
    w->packet = av_packet_alloc();
    if (w->stream == NULL || w->frame == NULL || w->packet == NULL) {
        ffwi_fail("out of memory");
        writer_free(w);
        return NULL;
    }
    /* No frame rate on the stream: the frames come when the window draws, and each is stamped with its own time.
     * The rate asked for was a hint for the encoder; declared here, Matroska would write it as every frame's
     * default duration, which a variable-rate recording would contradict on every frame. */
    w->stream->time_base = w->codec->time_base;
    code = avcodec_parameters_from_context(w->stream->codecpar, w->codec);
    if (code < 0) {
        ffwi_fail_av("could not describe the stream", code);
        writer_free(w);
        return NULL;
    }
    w->frame->format = w->codec->pix_fmt;
    w->frame->width = w->codec->width;
    w->frame->height = w->codec->height;
    w->frame->color_range = w->codec->color_range;
    w->frame->colorspace = w->codec->colorspace;
    w->frame->color_primaries = w->codec->color_primaries;
    w->frame->color_trc = w->codec->color_trc;
    code = av_frame_get_buffer(w->frame, 0);
    if (code < 0) {
        ffwi_fail_av("out of memory for a frame", code);
        writer_free(w);
        return NULL;
    }
    if (!converter(w, width, height, AV_PIX_FMT_RGBA)) {
        writer_free(w);
        return NULL;
    }

    code = avio_open(&w->format->pb, path, AVIO_FLAG_WRITE);
    if (code < 0) {
        ffwi_fail_av("could not create the file", code);
        writer_free(w);
        return NULL;
    }
    /* A recording outlives a crash, a kill or a pulled plug up to about its last second. Matroska keeps a cluster in
     * memory until it is closed, five seconds or 5 MB of them by default, and the file's own buffer holds more; so a
     * cluster is closed every second and each packet goes to the disk as it is written. What is lost is only what a
     * clean finish adds: the index of keyframes and the stated duration, which players do without. */
    AVDictionary *options = NULL;
    av_dict_set(&options, "cluster_time_limit", "1000", 0);
    w->format->flush_packets = 1;
    code = avformat_write_header(w->format, &options);
    av_dict_free(&options);
    if (code < 0) {
        ffwi_fail_av("could not write the file's header", code);
        writer_free(w);
        return NULL;
    }
    avio_flush(w->format->pb);
    w->header = 1;
    return w;
}

const char *ffw_writer_encoder(const FfwWriter *w) {
    return w->codec->codec->name;
}

/* Hand the encoder `frame` (NULL to finish) and write every packet it gives back. 1, or 0 on failure. */
static int encode(FfwWriter *w, AVFrame *frame) {
    int code = avcodec_send_frame(w->codec, frame);
    if (code < 0 && !(frame == NULL && code == AVERROR_EOF)) {
        ffwi_fail_av("could not encode a frame", code);
        return 0;
    }
    for (;;) {
        code = avcodec_receive_packet(w->codec, w->packet);
        if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) {
            return 1;
        }
        if (code < 0) {
            ffwi_fail_av("could not encode a frame", code);
            return 0;
        }
        av_packet_rescale_ts(w->packet, w->codec->time_base, w->stream->time_base);
        w->packet->stream_index = w->stream->index;
        code = av_interleaved_write_frame(w->format, w->packet);
        if (code < 0) {
            ffwi_fail_av("could not write to the file", code);
            return 0;
        }
    }
}

/* The canvas outside the picture, black: full-range Y 0 and the neutral chroma, in either layout. */
static void blacken(AVFrame *f) {
    int chroma_rows = (f->height + 1) / 2;
    memset(f->data[0], 0, (size_t) f->linesize[0] * f->height);
    memset(f->data[1], 128, (size_t) f->linesize[1] * chroma_rows);
    if (f->format == AV_PIX_FMT_YUV420P) {
        memset(f->data[2], 128, (size_t) f->linesize[2] * chroma_rows);
    }
}

int32_t ffw_writer_frame(FfwWriter *w, const uint8_t *pixels, uint32_t width, uint32_t height, int32_t order,
                         int64_t pts_us) {
    if (pixels == NULL || width == 0 || height == 0) {
        ffwi_fail("no pixels");
        return -1;
    }
    enum AVPixelFormat from = order == FFW_PIXELS_BGRA ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA;
    int64_t ms = pts_us / 1000;
    if (pts_us < 0 || ms <= w->last_ms) {
        return 0;   /* before the last frame, or in its millisecond: Matroska could not tell them apart */
    }
    /* Larger than the canvas: the part that fits, from the top left. */
    uint32_t shown_w = width < w->width ? width : w->width;
    uint32_t shown_h = height < w->height ? height : w->height;
    if (!converter(w, shown_w, shown_h, from)) {
        return -1;
    }
    /* The encoder may still hold the last frame's buffers; this gives a fresh one if so. */
    int code = av_frame_make_writable(w->frame);
    if (code < 0) {
        ffwi_fail_av("out of memory for a frame", code);
        return -1;
    }
    if (shown_w < w->width || shown_h < w->height) {
        blacken(w->frame);
    }
    const uint8_t *src[4] = {pixels, NULL, NULL, NULL};
    int stride[4] = {(int) width * 4, 0, 0, 0};
    if (sws_scale(w->scale, src, stride, 0, (int) shown_h, w->frame->data, w->frame->linesize) <= 0) {
        ffwi_fail("could not convert a frame from %s", av_get_pix_fmt_name(from));
        return -1;
    }
    w->frame->pts = ms;
    if (!encode(w, w->frame)) {
        return -1;
    }
    w->last_ms = ms;
    w->frames++;
    return 1;
}

int32_t ffw_writer_repeat(FfwWriter *w, int64_t pts_us) {
    int64_t ms = pts_us / 1000;
    if (w->frames == 0 || pts_us < 0 || ms <= w->last_ms) {
        return 0;
    }
    /* w->frame still holds the last frame's pixels: the encoder took its own reference to them, and nothing has
     * written to them since. Sent again, at a later time, it encodes to almost nothing. */
    w->frame->pts = ms;
    if (!encode(w, w->frame)) {
        return -1;
    }
    w->last_ms = ms;
    w->frames++;
    return 1;
}

int64_t ffw_writer_bytes(const FfwWriter *w) {
    int64_t n = avio_tell(w->format->pb);
    return n < 0 ? 0 : n;
}

int32_t ffw_writer_close(FfwWriter *w) {
    if (w == NULL) {
        return 1;
    }
    int ok = 1;
    if (w->header) {
        ok = encode(w, NULL);
        int code = av_write_trailer(w->format);
        if (code < 0 && ok) {
            ffwi_fail_av("could not finish the file", code);
            ok = 0;
        }
    }
    if (w->format != NULL && w->format->pb != NULL && w->format->pb->error < 0 && ok) {
        ffwi_fail_av("could not write to the file", w->format->pb->error);
        ok = 0;
    }
    writer_free(w);
    return ok;
}
