/*
 * level.c's leveller, which audio.c's decoder runs every sample through on its way to the ring: what it works
 * out is a gain for each sample, in dB, which the device applies, as far as the compression asked for, as it
 * plays. Nothing outside the shim sees it.
 */
#ifndef FFWRAP_LEVEL_H
#define FFWRAP_LEVEL_H

#include <stdint.h>

typedef struct FfwLevel FfwLevel;

/* Where the leveller hands on n stereo sample frames, each with its gain: 1, or 0 to stop (a seek or close came
 * first), and the push or finish that called it stops too and returns 0. */
typedef int (*FfwLevelSink)(void *to, const float *samples, const float *gains, int64_t n);

/* A leveller for stereo float at `rate`, or NULL out of memory. */
FfwLevel *ffwi_level_new(uint32_t rate);

/* Forget everything pushed and not yet handed on, and all that was heard before: a seek's fresh start. */
void ffwi_level_reset(FfwLevel *lv);

/* Take n sample frames (silence, for NULL) and hand on every one whose lookahead is now in. 1, or 0 if the sink
 * stopped it. */
int ffwi_level_push(FfwLevel *lv, const float *src, int64_t n, FfwLevelSink sink, void *to);

/* The stream has ended: hand on the rest, looking ahead only as far as there is. 1, or 0 if the sink stopped it. */
int ffwi_level_finish(FfwLevel *lv, FfwLevelSink sink, void *to);

void ffwi_level_free(FfwLevel *lv);

#endif /* FFWRAP_LEVEL_H */
