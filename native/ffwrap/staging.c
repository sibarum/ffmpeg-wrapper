/*
 * Decoding straight into memory the caller lends: a GPU's staging, mapped, so a decoded frame is where the GPU
 * copies from and the CPU copies nothing (ffwrap.h, "decoding into the caller's memory").
 *
 * The caller lends chunks; this cuts them into blocks of one size, each one frame laid out as the decoder wants
 * it, and hands a block to the decoder each time it asks for a frame's memory (get_buffer2). A block is free again
 * once nothing refers to its frame: not the decoder, which keeps reference frames, and not the caller, who keeps
 * frames to show. FFmpeg's buffer reference counting decides that, and this only keeps the free list.
 *
 * A decoder that cannot decode into memory it is given (see direct(): dav1d's AV1, unless FFmpeg carries
 * native/patches/libdav1d-get-buffer.patch) gets its own, and a frame kept from it is copied into a block once.
 * So is a frame decoded while no block was free: the decoder is never kept waiting, and never handed memory of
 * another layout, which some decoders refuse.
 */
#include "internal.h"

#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>

#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK ffw_lock;
#define LOCK_INIT(l) InitializeSRWLock(l)
#define LOCK(l) AcquireSRWLockExclusive(l)
#define UNLOCK(l) ReleaseSRWLockExclusive(l)
#define LOCK_FREE(l) ((void) 0)
#else
#include <pthread.h>
typedef pthread_mutex_t ffw_lock;
#define LOCK_INIT(l) pthread_mutex_init(l, NULL)
#define LOCK(l) pthread_mutex_lock(l)
#define UNLOCK(l) pthread_mutex_unlock(l)
#define LOCK_FREE(l) pthread_mutex_destroy(l)
#endif

#define MAX_CHUNKS 64
/* Every plane and every block starts at a multiple of this: wider than any CPU's SIMD wants, and than a GPU's
 * copy needs (a texel, at most 4 bytes). */
#define ALIGN 64
#define BLOCK_ALIGN 4096

typedef struct Chunk {
    uint8_t *base;
    size_t bytes;
    size_t carved;      /* blocks are cut from the front, as they are first needed */
    int32_t id;         /* the caller's name for it */
} Chunk;

struct FfwStaging {
    ffw_lock lock;
    int refs;           /* the video's own, and one per block or heap frame given out */

    /* The shape every block holds, set by the first frame asked for: 0 until then. */
    int w, h, format;
    int linesize[4];
    size_t offset[4];
    size_t block;

    Chunk chunks[MAX_CHUNKS];
    int nchunks;
    uint8_t **free;     /* blocks given back */
    int nfree, capfree;
    int64_t copies;     /* frames kept by copying them into a block */
};

FfwStaging *ffwi_staging_new(void) {
    FfwStaging *s = av_mallocz(sizeof *s);
    if (s == NULL) {
        return NULL;
    }
    LOCK_INIT(&s->lock);
    s->refs = 1;
    return s;
}

/* Under the lock: one reference fewer, and the whole freed with the last. Whether it was freed. */
static int unref_locked(FfwStaging *s) {
    if (--s->refs > 0) {
        return 0;
    }
    UNLOCK(&s->lock);
    LOCK_FREE(&s->lock);
    av_free(s->free);
    av_free(s);
    return 1;
}

void ffwi_staging_drop(FfwStaging *s) {
    if (s == NULL) {
        return;
    }
    LOCK(&s->lock);
    if (!unref_locked(s)) {
        UNLOCK(&s->lock);
    }
}

int ffwi_staging_add(FfwStaging *s, uint8_t *base, size_t bytes, int32_t id) {
    if (base == NULL || bytes == 0) {
        ffwi_fail("no memory to stage into");
        return 0;
    }
    if (((uintptr_t) base) % ALIGN != 0) {
        ffwi_fail("staging at %p is not aligned to %d bytes", (void *) base, ALIGN);
        return 0;
    }
    LOCK(&s->lock);
    if (s->nchunks == MAX_CHUNKS) {
        UNLOCK(&s->lock);
        ffwi_fail("already staging into %d chunks", MAX_CHUNKS);
        return 0;
    }
    s->chunks[s->nchunks++] = (Chunk) {base, bytes, 0, id};
    UNLOCK(&s->lock);
    return 1;
}

/* Under the lock: the layout of a w x h frame of `format` as `c` decodes it, as FFmpeg's own frame pool lays it
 * out (libavcodec/get_buffer.c), with every plane and line aligned to ALIGN. 0 when FFmpeg cannot say. */
static int shape_locked(FfwStaging *s, AVCodecContext *c, int w, int h, int format) {
    int stride_align[AV_NUM_DATA_POINTERS] = {0};
    int aw = w;
    int ah = h;
    avcodec_align_dimensions2(c, &aw, &ah, stride_align);
    int align = ALIGN;
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
        if (stride_align[i] > align) {
            align = stride_align[i];
        }
    }
    int linesize[4];
    for (;;) {
        if (av_image_fill_linesizes(linesize, (enum AVPixelFormat) format, aw) < 0) {
            return 0;
        }
        int unaligned = 0;
        for (int i = 0; i < 4; i++) {
            unaligned |= linesize[i] % align;
        }
        if (!unaligned) {
            break;
        }
        aw += aw & ~(aw - 1);
    }
    ptrdiff_t lines[4];
    for (int i = 0; i < 4; i++) {
        lines[i] = linesize[i];
    }
    size_t sizes[4];
    if (av_image_fill_plane_sizes(sizes, (enum AVPixelFormat) format, ah, lines) < 0) {
        return 0;
    }
    size_t at = 0;
    for (int i = 0; i < 4; i++) {
        s->offset[i] = at;
        s->linesize[i] = linesize[i];
        at += FFALIGN(sizes[i], (size_t) align);
    }
    at += 16 + ALIGN;   /* read past the end by SIMD, as FFmpeg's pool allows */
    s->block = FFALIGN(at, (size_t) BLOCK_ALIGN);
    s->w = w;
    s->h = h;
    s->format = format;
    return 1;
}

/* Under the lock: a free block, carved from a chunk if none was given back, or NULL. Counted as a reference. */
static uint8_t *take_locked(FfwStaging *s) {
    uint8_t *b = NULL;
    if (s->nfree > 0) {
        b = s->free[--s->nfree];
    } else {
        for (int i = 0; i < s->nchunks && b == NULL; i++) {
            Chunk *c = &s->chunks[i];
            if (c->bytes - c->carved >= s->block) {
                b = c->base + c->carved;
                c->carved += s->block;
            }
        }
    }
    if (b != NULL) {
        s->refs++;
    }
    return b;
}

/* A block's frame is unreferenced everywhere: free for the next. */
static void give_back(void *opaque, uint8_t *data) {
    FfwStaging *s = opaque;
    LOCK(&s->lock);
    if (s->nfree == s->capfree) {
        int cap = s->capfree ? s->capfree * 2 : 32;
        uint8_t **grown = av_realloc_array(s->free, cap, sizeof *grown);
        if (grown == NULL) {
            /* Lost to this video, not to the process: the caller frees the chunk. */
            if (!unref_locked(s)) {
                UNLOCK(&s->lock);
            }
            return;
        }
        s->free = grown;
        s->capfree = cap;
    }
    s->free[s->nfree++] = data;
    if (!unref_locked(s)) {
        UNLOCK(&s->lock);
    }
}

/* A frame's memory from the heap, laid out as a block is: for a decoder asking while no block is free. */
static void heap_free(void *opaque, uint8_t *data) {
    (void) data;
    FfwStaging *s = ((FfwStaging **) opaque)[0];
    av_free(opaque);
    ffwi_staging_drop(s);
}

/* Fill f's planes from a block-shaped memory at `b`. */
static void lay_out(const FfwStaging *s, AVFrame *f, uint8_t *b) {
    for (int i = 0; i < 4; i++) {
        f->data[i] = s->linesize[i] ? b + s->offset[i] : NULL;
        f->linesize[i] = s->linesize[i];
    }
    f->extended_data = f->data;
}

/*
 * Whether c may be handed memory of the staging's: a decoder that says it decodes into memory it is given (DR1), or
 * dav1d's, which asks only where FFmpeg carries native/patches/libdav1d-get-buffer.patch, and checks for itself
 * that what it is given will do. An unpatched dav1d never asks. Any other decoder gets FFmpeg's own memory.
 */
static int direct(const AVCodecContext *c) {
    return (c->codec->capabilities & AV_CODEC_CAP_DR1)
        || (c->codec->wrapper_name != NULL && strcmp(c->codec->wrapper_name, "libdav1d") == 0);
}

int ffwi_get_buffer(AVCodecContext *c, AVFrame *f, int flags) {
    FfwStaging *s = c->opaque;
    if (s == NULL || !direct(c)) {
        return avcodec_default_get_buffer2(c, f, flags);
    }
    LOCK(&s->lock);
    if (s->block == 0 && !shape_locked(s, c, f->width, f->height, f->format)) {
        UNLOCK(&s->lock);
        return avcodec_default_get_buffer2(c, f, flags);
    }
    if (f->format != s->format || f->width > s->w || f->height > s->h) {
        /* Another size or format than the stream began with: the decoder is setting up again anyway. */
        UNLOCK(&s->lock);
        return avcodec_default_get_buffer2(c, f, flags);
    }
    uint8_t *b = take_locked(s);
    size_t block = s->block;
    if (b != NULL) {
        UNLOCK(&s->lock);
        f->buf[0] = av_buffer_create(b, block, give_back, s, 0);
        if (f->buf[0] == NULL) {
            give_back(s, b);
            return AVERROR(ENOMEM);
        }
        lay_out(s, f, b);
        return 0;
    }
    s->refs++;
    UNLOCK(&s->lock);
    /* No block free: the heap, in the same layout, and the frame is copied into a block if it is kept. The
     * owner pointer goes first in the allocation, and the frame after it, aligned. */
    uint8_t *raw = av_malloc(block + 2 * ALIGN);
    if (raw == NULL) {
        ffwi_staging_drop(s);
        return AVERROR(ENOMEM);
    }
    ((FfwStaging **) raw)[0] = s;
    uint8_t *at = (uint8_t *) FFALIGN((uintptr_t) raw + sizeof(FfwStaging *), (uintptr_t) ALIGN);
    f->buf[0] = av_buffer_create(at, block, heap_free, raw, 0);
    if (f->buf[0] == NULL) {
        av_free(raw);
        ffwi_staging_drop(s);
        return AVERROR(ENOMEM);
    }
    lay_out(s, f, at);
    return 0;
}

/* Whether f's memory is one of s's blocks. */
static int in_block(const FfwStaging *s, const AVFrame *f) {
    return f->buf[0] != NULL && f->buf[1] == NULL && av_buffer_get_opaque(f->buf[0]) == (void *) s;
}

AVFrame *ffwi_staging_keep(FfwStaging *s, AVCodecContext *c, const AVFrame *f) {
    if (in_block(s, f)) {
        AVFrame *k = av_frame_clone(f);
        if (k == NULL) {
            ffwi_fail("out of memory for a frame");
        }
        return k;
    }
    LOCK(&s->lock);
    if (s->block == 0) {
        /* Never asked for a block (a decoder of its own memory): the frames' shape is this one's. A decoder that
         * asks would ask for its coded size, which may be larger, so that is the shape where it can ask. */
        int w = f->width;
        int h = f->height;
        if (c->codec->capabilities & AV_CODEC_CAP_DR1) {
            w = FFMAX(w, FFMAX(c->width, c->coded_width));
            h = FFMAX(h, FFMAX(c->height, c->coded_height));
        }
        if (!shape_locked(s, c, w, h, f->format)) {
            UNLOCK(&s->lock);
            ffwi_fail("no layout for a %dx%d frame of format %d", f->width, f->height, f->format);
            return NULL;
        }
    }
    if (f->format != s->format || f->width > s->w || f->height > s->h) {
        UNLOCK(&s->lock);
        ffwi_fail("a %dx%d frame does not fit the staging's %dx%d", f->width, f->height, s->w, s->h);
        return NULL;
    }
    uint8_t *b = take_locked(s);
    size_t block = s->block;
    if (b == NULL) {
        UNLOCK(&s->lock);
        ffwi_fail("no room in the staging");
        return NULL;
    }
    s->copies++;
    UNLOCK(&s->lock);
    AVFrame *k = av_frame_alloc();
    if (k != NULL) {
        k->buf[0] = av_buffer_create(b, block, give_back, s, 0);
    }
    if (k == NULL || k->buf[0] == NULL) {
        av_frame_free(&k);
        give_back(s, b);
        ffwi_fail("out of memory for a frame");
        return NULL;
    }
    k->format = f->format;
    k->width = f->width;
    k->height = f->height;
    lay_out(s, k, b);
    if (av_frame_copy_props(k, f) < 0 || av_frame_copy(k, f) < 0) {
        av_frame_free(&k);
        ffwi_fail("could not copy a frame into the staging");
        return NULL;
    }
    return k;
}

int64_t ffwi_staging_block(FfwStaging *s) {
    LOCK(&s->lock);
    int64_t b = (int64_t) s->block;
    UNLOCK(&s->lock);
    return b;
}

int32_t ffwi_staging_room(FfwStaging *s) {
    LOCK(&s->lock);
    int32_t room = -1;
    if (s->block > 0) {
        size_t n = (size_t) s->nfree;
        for (int i = 0; i < s->nchunks; i++) {
            n += (s->chunks[i].bytes - s->chunks[i].carved) / s->block;
        }
        room = n > INT32_MAX ? INT32_MAX : (int32_t) n;
    }
    UNLOCK(&s->lock);
    return room;
}

int64_t ffwi_staging_copies(FfwStaging *s) {
    LOCK(&s->lock);
    int64_t n = s->copies;
    UNLOCK(&s->lock);
    return n;
}

int32_t ffwi_kept_place(const AVFrame *f, int64_t offsets[4], int32_t strides[4]) {
    FfwStaging *s = f->buf[0] != NULL ? av_buffer_get_opaque(f->buf[0]) : NULL;
    if (s == NULL) {
        return -1;
    }
    LOCK(&s->lock);
    int32_t id = -1;
    for (int i = 0; i < s->nchunks; i++) {
        const Chunk *c = &s->chunks[i];
        if (f->data[0] >= c->base && f->data[0] < c->base + c->bytes) {
            id = c->id;
            for (int p = 0; p < 4; p++) {
                offsets[p] = f->data[p] != NULL ? (int64_t) (f->data[p] - c->base) : 0;
                strides[p] = f->linesize[p];
            }
            break;
        }
    }
    UNLOCK(&s->lock);
    return id;
}
