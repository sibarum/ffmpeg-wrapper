/*
 * ffwrap — one C ABI over every video FFmpeg can decode, the sound that goes with it, and AV1 recorded
 * from frames as they are made.
 *
 * Hand-written, not generated: this header is the contract, and the C in ../ffwrap/ (ffwrap.c for
 * pictures, audio.c for sound, encode.c for recording) and the Java downcalls in
 * ../../src/main/java/sibarum/ffmpeg/CApi.java are all written against it. Thirty-five functions, no
 * structs crossing the boundary, three opaque handles. FFmpeg's own structs change layout between major versions; they stay on this side of the
 * line, so the Java never learns one.
 *
 * Pixels are always straight RGBA8, sRGB, tightly packed, top row first: what imagelib gives a picture,
 * so a video frame uploads the way a picture does. Sound never crosses on its way out: it goes from the
 * decoder to the speaker on this side, and only its clock, and a copy of the last of it for a meter, come
 * back.
 *
 * Failure is uniform: a function returning a pointer returns NULL, one returning a status returns 0 (or
 * -1 where 0 already means something), and ffw_last_error() then says why on this thread.
 *
 * An FfwVideo and an FfwWriter are used from one thread at a time; an FfwAudio from any. Different handles on different
 * threads do not interact.
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

/* 1 if the file also holds an audio stream, else 0. An FfwVideo does not decode it; ffw_audio_open does. */
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

/* --- sound -------------------------------------------------------------------------------------------
 *
 * A file's sound, decoded and played out of a speaker: a second handle on the same file, independent of
 * any FfwVideo on it, so a video's exact, frame-numbered seeking is never at the sound's mercy.
 *
 * An FfwAudio decodes on a thread of its own, a little ahead, into a buffer the output device drains;
 * its calls only steer that, and may come from any thread. Its clock is the time of the sound being
 * heard now, which is what a player shows its pictures by. */
typedef struct FfwAudio FfwAudio;

/* Where an FfwAudio plays to. */
#define FFW_AUDIO_SPEAKERS 0    /* the system's default output, following it when that changes */
#define FFW_AUDIO_NOWHERE  1    /* no device: the sound is drained in real time and dropped (for tests) */

/* Open the sound of the file at a UTF-8 path (the stream that goes with its best video, if it has one)
 * and an output, paused at the start. NULL if the file will not open, has no sound or no decoder for
 * it, or the output will not open. Close with ffw_audio_close. */
FFW_API FfwAudio *ffw_audio_open(const char *path, int32_t output);

/* The sound's codec's short name ("aac"), owned by a; its channels and samples per second as stored. */
FFW_API const char *ffw_audio_codec(const FfwAudio *a);
FFW_API uint32_t ffw_audio_channels(const FfwAudio *a);
FFW_API uint32_t ffw_audio_rate(const FfwAudio *a);

/* Start and stop the output. Paused, the clock holds and the device is given back. 1, or 0 on failure. */
FFW_API int32_t ffw_audio_play(FfwAudio *a);
FFW_API int32_t ffw_audio_pause(FfwAudio *a);

/* Go to `us` from the stream's start, exactly: the clock says `us` at once and holds there until the
 * sound from `us` on has been decoded and starts to be heard. Playing or paused, it stays so. 1, or 0. */
FFW_API int32_t ffw_audio_seek(FfwAudio *a, int64_t us);

/* Loudness, from 0 (silent) to 1 (as stored); clamped to that. */
FFW_API void ffw_audio_volume(FfwAudio *a, float volume);

/* Dynamic range compression, from 0 (as stored, the default) to 1; clamped to that. Loud passages are
 * lowered and quiet ones raised towards one loudness, and peaks limited to -6 dBFS, each sample's gain
 * worked out with the decoded sound after it in view, so the gain comes down before a loud passage or a
 * bang arrives. At 1 no sample is played above -6 dBFS; at less, none below it is raised past it. The
 * clock is unchanged by it, and a change takes effect at once. */
FFW_API void ffw_audio_compression(FfwAudio *a, float amount);

/* Copy the last `frames` stereo sample frames given to the output, oldest first, as float pairs, into dst
 * (2 * frames floats): what is being heard, compressed but before the volume. Up to a quarter of a second
 * is kept. The number copied: fewer when less has been played since the open. */
FFW_API uint32_t ffw_audio_heard(FfwAudio *a, float *dst, uint32_t frames);

/* The time of the sound being heard now, in microseconds from the stream's start: smooth between the
 * device's periods, and allowing for what the device holds but has not played yet. */
FFW_API int64_t ffw_audio_clock_us(FfwAudio *a);

/* 1 once all the sound has been played (until the next seek), else 0. */
FFW_API int32_t ffw_audio_ended(FfwAudio *a);

/* Stop the output, end the decoding thread and free everything. NULL does nothing. */
FFW_API void ffw_audio_close(FfwAudio *a);

/* --- recording -----------------------------------------------------------------------------------------
 *
 * Frames in, as they are made, and AV1 out: a window recorded while it runs. Each frame carries its own
 * time, which the container keeps, so frames need not come at a steady rate and a pause is recorded as
 * long as it was. Pixels in are what ffw_pixels gives out: straight RGBA8, sRGB, tightly packed, top row
 * first. They are stored as BT.709 4:2:0 at full range, and tagged so. */
typedef struct FfwWriter FfwWriter;

/* Which AV1 encoder records. */
#define FFW_ENCODE_SOFTWARE 0   /* SVT-AV1 in its real-time mode: on every machine, the same file on each */
#define FFW_ENCODE_HARDWARE 1   /* the GPU's (NVENC, Quick Sync, AMF), whichever opens first; else SVT-AV1 */

/* Create the file at a UTF-8 path, its container told by its name (.mkv or .webm), on a canvas of
 * width x height: every frame in the file is that size, whatever size each frame handed in is (a window
 * being resized), so it is the largest a frame will be, such as the screen's. Frames come about `fps` a
 * second (a hint for rate control, not a schedule). `quality` is AV1's
 * quantiser scale, 0 (best) to 63 (smallest). NULL if no encoder opens or the file cannot be created.
 * Finish it with ffw_writer_close. */
FFW_API FfwWriter *ffw_writer_open(const char *path, uint32_t width, uint32_t height, double fps,
                                   int32_t quality, int32_t encoder);

/* The encoder that opened ("libsvtav1", "av1_nvenc"), owned by w. */
FFW_API const char *ffw_writer_encoder(const FfwWriter *w);

/* The byte order of the pixels handed to ffw_writer_frame. */
#define FFW_PIXELS_RGBA 0       /* what ffw_pixels gives out, and what imagelib gives a picture */
#define FFW_PIXELS_BGRA 1       /* what a swapchain and a DXGI back buffer hold: a presented frame as read back */

/* Encode width * height * 4 bytes of pixels in `order`, shown at `pts_us` from the recording's start,
 * and write what the encoder gives back. The frame goes at the canvas's top left, unscaled; the rest of
 * the canvas is black, and what falls outside it is cut off. 1; 0 if it was dropped for coming no later
 * than the last frame's millisecond (the container's resolution); -1 on failure. The pixels are not
 * kept. */
FFW_API int32_t ffw_writer_frame(FfwWriter *w, const uint8_t *pixels, uint32_t width, uint32_t height,
                                 int32_t order, int64_t pts_us);

/* Encode the last frame again, shown at `pts_us`: a picture that stayed on the screen, so the file lasts
 * until then rather than ending where the last change was. 1; 0 if there is no frame yet, or `pts_us` is
 * no later than the last frame's millisecond; -1 on failure. */
FFW_API int32_t ffw_writer_repeat(FfwWriter *w, int64_t pts_us);

/* Bytes written to the file so far. */
FFW_API int64_t ffw_writer_bytes(const FfwWriter *w);

/* Encode what the encoder still holds, finish the file and free everything. 1 if the file is whole, else
 * 0, with w freed all the same. NULL does nothing and returns 1. */
FFW_API int32_t ffw_writer_close(FfwWriter *w);

/* Why the last call on this thread failed, or NULL. Valid until the next failing call on it. */
FFW_API const char *ffw_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* FFWRAP_H */
