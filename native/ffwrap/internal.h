/*
 * What ffwrap.c, audio.c and staging.c share, and nothing outside the shim sees: the error and log plumbing
 * behind ffw_last_error, and the staging a video decodes into.
 */
#ifndef FFWRAP_INTERNAL_H
#define FFWRAP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* Say why the call in hand failed, for ffw_last_error on this thread. */
void ffwi_fail(const char *fmt, ...);

/* The same, in FFmpeg's words for `code`, after what this was doing, with the last error FFmpeg logged. */
void ffwi_fail_av(const char *doing, int code);

/* Keep FFmpeg's log from here on rather than print it, and forget what this thread kept before. */
void ffwi_listen(void);

/* staging.c: memory the caller lends, decoded straight into. One per FfwVideo, made when it opens; freed once
 * the video has dropped it and no frame refers to a block of it. */
struct AVCodecContext;
struct AVFrame;
typedef struct FfwStaging FfwStaging;

FfwStaging *ffwi_staging_new(void);
void ffwi_staging_drop(FfwStaging *s);
int ffwi_staging_add(FfwStaging *s, uint8_t *base, size_t bytes, int32_t id);
/* The decoder's get_buffer2, with the staging as the codec context's opaque. */
int ffwi_get_buffer(struct AVCodecContext *c, struct AVFrame *f, int flags);
/* A new reference to f in a block: f's own when it was decoded there, else a copy into a free one. NULL, with
 * the reason said, when no block is free or f does not fit one. */
struct AVFrame *ffwi_staging_keep(FfwStaging *s, struct AVCodecContext *c, const struct AVFrame *f);
int64_t ffwi_staging_block(FfwStaging *s);
int32_t ffwi_staging_room(FfwStaging *s);
int64_t ffwi_staging_copies(FfwStaging *s);
/* Which chunk a kept frame is in, by the caller's id, and each plane's offset into it and stride; -1 if none. */
int32_t ffwi_kept_place(const struct AVFrame *f, int64_t offsets[4], int32_t strides[4]);

#endif /* FFWRAP_INTERNAL_H */
