/*
 * ffrecord — AV1 recorded from frames as they are made, and nothing else: no decoder, no demuxer, no sound.
 *
 * The recording half of the wrapper, cut from ffwrap so an application that only records carries only an encoder.
 * It is built against its own FFmpeg (../record.sh), configured with the AV1 encoders, the Matroska and WebM muxers
 * and the file protocol alone, and named with a -rec suffix (avcodec-rec-62.dll, libavcodec-rec.so.62) so it never
 * meets the decoding build's libraries, which an application that both plays and records loads beside it. Its
 * functions are ffr_, not ffw_, for the same reason: both shims are found through one class loader's symbols.
 *
 * Hand-written, not generated: this header is the contract, and record.c and the Java downcalls in
 * ffmpeg-wrapper-record's RecordApi.java are written against it. Eight functions, no structs crossing the boundary,
 * one opaque handle.
 *
 * Failure is uniform: a function returning a pointer returns NULL, one returning a status returns 0 (or -1 where 0
 * already means something), and ffr_last_error() then says why on this thread.
 *
 * An FfrWriter is used from one thread at a time. Different handles on different threads do not interact.
 */
#ifndef FFRECORD_H
#define FFRECORD_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define FFR_API __declspec(dllexport)
#else
#define FFR_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* FFmpeg's version string, as this build of it reports it. */
FFR_API const char *ffr_version(void);

/* Frames in, as they are made, and AV1 out: a window recorded while it runs. Each frame carries its own time, which
 * the container keeps, so frames need not come at a steady rate and a pause is recorded as long as it was. Pixels
 * in are straight RGBA8 (or BGRA8), sRGB, tightly packed, top row first. They are stored as BT.709 4:2:0 at full
 * range, and tagged so. */
typedef struct FfrWriter FfrWriter;

/* Which AV1 encoder records. */
#define FFR_ENCODE_SOFTWARE 0   /* SVT-AV1 in its real-time mode: on every machine, the same file on each */
#define FFR_ENCODE_HARDWARE 1   /* the GPU's (NVENC, Quick Sync, AMF), whichever opens first; else SVT-AV1 */

/* Create the file at a UTF-8 path, its container told by its name (.mkv or .webm), on a canvas of
 * width x height: every frame in the file is that size, whatever size each frame handed in is (a window
 * being resized), so it is the largest a frame will be, such as the screen's. Frames come about `fps` a
 * second (a hint for rate control, not a schedule). `quality` is AV1's
 * quantiser scale, 0 (best) to 63 (smallest). NULL if no encoder opens or the file cannot be created.
 * Finish it with ffr_close. */
FFR_API FfrWriter *ffr_open(const char *path, uint32_t width, uint32_t height, double fps, int32_t quality,
                            int32_t encoder);

/* The encoder that opened ("libsvtav1", "av1_nvenc"), owned by w. */
FFR_API const char *ffr_encoder(const FfrWriter *w);

/* The byte order of the pixels handed to ffr_frame. */
#define FFR_PIXELS_RGBA 0       /* what a decoded frame and imagelib's pictures hold */
#define FFR_PIXELS_BGRA 1       /* what a swapchain and a DXGI back buffer hold: a presented frame as read back */

/* Encode width * height * 4 bytes of pixels in `order`, shown at `pts_us` from the recording's start,
 * and write what the encoder gives back. The frame goes at the canvas's top left, unscaled; the rest of
 * the canvas is black, and what falls outside it is cut off. 1; 0 if it was dropped for coming no later
 * than the last frame's millisecond (the container's resolution); -1 on failure. The pixels are not
 * kept. */
FFR_API int32_t ffr_frame(FfrWriter *w, const uint8_t *pixels, uint32_t width, uint32_t height, int32_t order,
                          int64_t pts_us);

/* Encode the last frame again, shown at `pts_us`: a picture that stayed on the screen, so the file lasts
 * until then rather than ending where the last change was. 1; 0 if there is no frame yet, or `pts_us` is
 * no later than the last frame's millisecond; -1 on failure. */
FFR_API int32_t ffr_repeat(FfrWriter *w, int64_t pts_us);

/* Bytes written to the file so far. */
FFR_API int64_t ffr_bytes(const FfrWriter *w);

/* Encode what the encoder still holds, finish the file and free everything. 1 if the file is whole, else
 * 0, with w freed all the same. NULL does nothing and returns 1. */
FFR_API int32_t ffr_close(FfrWriter *w);

/* Why the last call on this thread failed, or NULL. Valid until the next failing call on it. */
FFR_API const char *ffr_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* FFRECORD_H */
