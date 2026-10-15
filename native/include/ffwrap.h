/*
 * ffwrap — one C ABI over every video FFmpeg can decode.
 *
 * Hand-written, not generated: this header is the contract, and the C in ../ffwrap/ffwrap.c and the
 * Java downcalls in ../../src/main/java/sibarum/ffmpeg/CApi.java are both written against it. Sixteen
 * functions, no structs crossing the boundary, one opaque handle. FFmpeg's own structs change layout
 * between major versions; they stay on this side of the line, so the Java never learns one.
 *
 * Pixels are always straight RGBA8, sRGB, tightly packed, top row first: what imagelib gives a picture,
 * so a video frame uploads the way a picture does.
 *
 * Failure is uniform: a function returning a pointer returns NULL, one returning a status returns 0 (or
 * -1 where 0 already means something), and ffw_last_error() then says why on this thread.
 *
 * A handle is used from one thread at a time. Different handles on different threads do not interact.
 */
#ifndef FFWRAP_H
#define FFWRAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define FFW_API __declspec(dllexport)
#else
#define FFW_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* An open file and its best video stream, decoding forward from wherever it was last sought. */
typedef struct FfwVideo FfwVideo;

/* FFmpeg's own version string, for a log line. Never NULL. */
FFW_API const char *ffw_version(void);

/* Open the file at a UTF-8 path and pick its best video stream. NULL if it will not open, has no video
 * stream, or no decoder for it. Close with ffw_close. */
FFW_API FfwVideo *ffw_open(const char *path);

/* The size a frame is shown at, in px: the coded size with the sample aspect applied to the width. */
FFW_API uint32_t ffw_width(const FfwVideo *v);
FFW_API uint32_t ffw_height(const FfwVideo *v);

/* The whole file's length in microseconds, or -1 where the container does not say. */
FFW_API int64_t ffw_duration_us(const FfwVideo *v);

/* Frames per second as the stream declares it, or 0 where it does not (a variable rate may still say). */
FFW_API double ffw_frame_rate(const FfwVideo *v);

/* The video codec's and the container's short names ("h264", "mov,mp4,m4a,3gp,3g2,mj2"). Owned by v. */
FFW_API const char *ffw_codec(const FfwVideo *v);
FFW_API const char *ffw_container(const FfwVideo *v);

/* 1 if the file also holds an audio stream, else 0. Nothing here decodes it. */
FFW_API int32_t ffw_has_audio(const FfwVideo *v);

/* Scale every frame from now on to width x height px. The default is ffw_width x ffw_height.
 * Returns 1 on success, 0 on failure. */
FFW_API int32_t ffw_output(FfwVideo *v, uint32_t width, uint32_t height);

/* Decode the next frame. 1 if there is one (read it with ffw_pixels and ffw_pts_us), 0 at the end of
 * the stream, -1 on failure. */
FFW_API int32_t ffw_next(FfwVideo *v);

/* The frame ffw_next last produced: its time from the stream's start in microseconds, and its
 * output_width * output_height * 4 bytes, owned by v and valid until the next call on v. */
FFW_API int64_t ffw_pts_us(const FfwVideo *v);
FFW_API const uint8_t *ffw_pixels(const FfwVideo *v);

/* Go to `us` from the stream's start: the next ffw_next gives the first frame shown at or after it.
 * Returns 1 on success, 0 on failure. */
FFW_API int32_t ffw_seek(FfwVideo *v, int64_t us);

FFW_API void ffw_close(FfwVideo *v);

/* Why the last call on this thread failed, or NULL. Valid until the next failing call on it. */
FFW_API const char *ffw_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* FFWRAP_H */
