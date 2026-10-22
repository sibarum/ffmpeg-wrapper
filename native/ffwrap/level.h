/*
 * level.c's compression, in two halves. The decoder's half measures: every sample on its way into the ring
 * gets three numbers worked out with the decoded sound around it, before and after in view. The device's half
 * decides: as each sample is played, one knob turns those numbers into its gain, so turning the knob is heard
 * at once. Nothing outside the shim sees either.
 */
#ifndef FFWRAP_LEVEL_H
#define FFWRAP_LEVEL_H

#include <stdint.h>

/* What the decoder's half gives each sample frame: the passage's loudness measured slowly and quickly (LUFS),
 * and its peak (dBFS), each looking ahead so that it rises before the sound does. */
#define FFWI_LEVEL_MEASURES 3

typedef struct FfwLevel FfwLevel;

/* Where the leveller hands on n stereo sample frames, each with its FFWI_LEVEL_MEASURES measures: 1, or 0 to
 * stop (a seek or close came first), and the push or finish that called it stops too and returns 0. */
typedef int (*FfwLevelSink)(void *to, const float *samples, const float *measures, int64_t n);

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

/* The knob, from 0 to 1, as the settings it stands for: worked out when it turns, read for every sample. */
typedef struct {
    int on;                         /* 0 at 0: played as stored */
    float quick;                    /* how far the quick loudness stands in for the slow one */
    float band;                     /* LU either side of the target left alone */
    float slope;                    /* towards the target: 1 - 1/ratio */
    float raise, cut;               /* the most a gain may be raised, and lowered, dB */
    float floor;                    /* LUFS: a passage at L is raised at most L - floor */
    float ceiling;                  /* dBFS: no sample is played above it */
    float release, guard_release;   /* dB a sample frame, at most, that each comes back up by */
} FfwLevelKnob;

void ffwi_level_knob(FfwLevelKnob *knob, float amount, uint32_t rate);

/* The device's half's memory, for the releases: cleared (zeroed) at a seek. */
typedef struct {
    int started;
    float level, guard;
} FfwLevelHeld;

/* A sample frame's gain in dB, from its measures, the knob, and what came before. */
float ffwi_level_gain(const FfwLevelKnob *knob, const float *measures, FfwLevelHeld *held);

#endif /* FFWRAP_LEVEL_H */
