/*
 * level.c — dynamic range compression with true lookahead, on one knob: loud and quiet brought together, the
 * way a TV's night mode does, so the dialogue can be heard without the explosions waking the house.
 *
 * Lookahead without delay. A compressor on a live signal can only see ahead by holding the signal back, and
 * everything else waits with it. A file is not live: the decoder runs ahead of the device by half a second
 * already, so the future is decoded before it is due. The decoder's half takes the samples the decoder makes,
 * measures each one with the sound around it, before and after, in view, and hands it on once enough of what
 * follows has been decoded. The holding back is in the decoder, which is ahead anyway; every sample keeps its
 * place in the stream's time, and the clock (which counts samples played) does not move. After a seek the first
 * sound waits for 0.37 s of stream to be decoded, which takes about a millisecond.
 *
 * Measures, not gains. What the decoder's half hands on with each sample is how loud the passage around it is
 * and how high its peak, not what to do about it. The device's half turns those into a gain as the sample is
 * played, by the knob as it is then, so every setting the knob stands for (ratio, threshold, how far quiet
 * passages are raised, the ceiling, the release) changes at once, mid-drag, with nothing decoded again. The
 * measures look ahead the way the gains used to: a gain that falls as loudness rises, taken at the loudest
 * point near a sample, is the least gain near it, so the gain comes down before a loud passage, not after.
 *
 * Three measures for every sample frame:
 *
 *   Slow loudness. A 10 ms block's loudness is its K-weighted mean square, as ITU-R BS.1770 measures it. The
 *   passage's is the median block's over 0.31 s centred on it: its sustained level, which a bang or a pause
 *   between words does not move (a bang is the peak's to deal with, and a pause nobody's). That is dilated
 *   (each the loudest over 0.21 s around it) and averaged over the same span, then laid straight between blocks.
 *
 *   Quick loudness. The same over 50 ms, dilated and averaged over 50 ms: it follows syllables. The knob's top
 *   quarter levels by it, which pumps, audibly; that is what the top of the knob is for.
 *
 *   Peak. Each frame's peak in dBFS, dilated over HOLD either side (long enough to span a cycle of the lowest
 *   bass, so the gain does not follow the waveform) and averaged over RAMP either side, so it is up before the
 *   peak arrives. Every value averaged is at least the frame's own peak, so the measure is too, and a gain of
 *   ceiling - peak keeps the frame at or under the ceiling.
 *
 * The knob runs from 0, as stored, through night mode at 0.5, to too much at 1, on purpose: turned all the way
 * up the effect cannot be missed, which is how an ear learns to hear it lower down. It never does harm: at any
 * setting but 0 no sample is played above the ceiling, which is never above 0 dBFS, and every step that shapes
 * a gain only lowers it. See ffwi_level_knob for what each setting is along the way.
 */
#include "level.h"

#include <math.h>
#include <string.h>

#include <libavutil/mem.h>

#define CHANNELS 2
#define MEASURES FFWI_LEVEL_MEASURES

#define TARGET      -18.0f  /* LUFS that the leveller pulls towards */
#define SILENT      -120.0f /* dB that silence is measured at */
#define WINDOW      15      /* blocks either side that the slow loudness takes its median over */
#define SPREAD      10      /* blocks either side that it is dilated over, and averaged over */
#define QUICK       2       /* blocks either side that the quick loudness takes its median, dilates and averages */
#define BLOCKS      128     /* blocks kept: the lookahead (37) and the history (36) fit with room */
#define HOLD        100     /* the peak is dilated over 1/HOLD s either side: 10 ms, a whole cycle at 50 Hz in all */
#define RAMP        500     /* and averaged over 1/RAMP s either side: 2 ms */

/* A biquad over each channel, direct form II transposed. */
typedef struct {
    double b0, b1, b2, a1, a2;
    double z1[CHANNELS], z2[CHANNELS];
} Biquad;

/* A measure per block, worked out from the one before it over `reach` blocks either side. */
typedef struct {
    float *at;                      /* at b % BLOCKS */
    int64_t made;                   /* blocks with it worked out */
} Stage;

struct FfwLevel {
    int block;                      /* sample frames a block: 10 ms */
    int hold, ramp;                 /* sample frames either side the peak is dilated over, and averaged over */
    int64_t cap;                    /* sample frames kept: BLOCKS blocks */

    /* Per sample frame, at f % cap. */
    float *samples;                 /* CHANNELS each, as pushed */
    float *peak;                    /* its own peak, dBFS */
    float *dilated;                 /* the highest peak within hold of it */
    int64_t *deque;                 /* the frames that may yet be the highest in the dilation's window, falling */
    int deque_cap, deque_head, deque_size;
    int64_t deque_next;             /* the next frame to enter the dilation's window */
    int64_t n_dilated;              /* frames with dilated worked out */

    /* Per block. */
    float *lufs;                    /* its own loudness */
    Stage median, most, slow;       /* the slow loudness, step by step */
    Stage q_median, q_most, quick;  /* the quick */

    float *chunk;                   /* a block's measures, handed on */
    Biquad shelf, pass;             /* K-weighting: the head's high shelf, then a high pass */
    double sum;                     /* the block being pushed: its K-weighted squares so far */

    int64_t in, out;                /* sample frames pushed, and handed on, since the reset */
    int64_t total;                  /* sample frames in all once finished, else -1 */
    int64_t box_lo, box_hi;         /* the dilated frames summed in box, inclusive */
    double box;
};

/* --- K-weighting (ITU-R BS.1770), at any rate: the filters' analogue prototypes, as libebur128 derives them -- */

static void k_weighting(FfwLevel *lv, double rate) {
    const double pi = 3.14159265358979323846;
    double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
    double k = tan(pi * f0 / rate);
    double vh = pow(10.0, g / 20.0);
    double vb = pow(vh, 0.4996667741545416);
    double a0 = 1.0 + k / q + k * k;
    lv->shelf.b0 = (vh + vb * k / q + k * k) / a0;
    lv->shelf.b1 = 2.0 * (k * k - vh) / a0;
    lv->shelf.b2 = (vh - vb * k / q + k * k) / a0;
    lv->shelf.a1 = 2.0 * (k * k - 1.0) / a0;
    lv->shelf.a2 = (1.0 - k / q + k * k) / a0;

    f0 = 38.13547087602444;
    q = 0.5003270373238773;
    k = tan(pi * f0 / rate);
    a0 = 1.0 + k / q + k * k;
    lv->pass.b0 = 1.0;
    lv->pass.b1 = -2.0;
    lv->pass.b2 = 1.0;
    lv->pass.a1 = 2.0 * (k * k - 1.0) / a0;
    lv->pass.a2 = (1.0 - k / q + k * k) / a0;
}

static double filter(Biquad *f, int c, double x) {
    double y = f->b0 * x + f->z1[c];
    f->z1[c] = f->b1 * x - f->a1 * y + f->z2[c];
    f->z2[c] = f->b2 * x - f->a2 * y;
    return y;
}

/* --- the decoder's half ----------------------------------------------------------------------------------- */

static int ended(const FfwLevel *lv) {
    return lv->total >= 0;
}

/* Blocks whose loudness is in: the whole ones, and once finished the last part one too. */
static int64_t blocks(const FfwLevel *lv) {
    return ended(lv) ? (lv->total + lv->block - 1) / lv->block : lv->in / lv->block;
}

enum { MEDIAN, MOST, MEAN };

/* Work out `to` for every block whose window of `from` (`have` of them in) is in: each the median, the most, or
 * the mean over `reach` either side. Once finished, a window looks only as far as there is. */
static void stage(FfwLevel *lv, const float *from, int64_t have, Stage *to, int reach, int how) {
    int64_t nb = blocks(lv);
    int done = ended(lv);
    for (int64_t b; (b = to->made) < nb && (b + reach < have || (done && have == nb)); to->made++) {
        int64_t lo = b - reach < 0 ? 0 : b - reach;
        int64_t hi = b + reach < have ? b + reach : have - 1;
        float v;
        if (how == MEDIAN) {
            /* An insertion sort of 31 at most, a hundred times a second. */
            float sorted[2 * WINDOW + 1];
            int n = 0;
            for (int64_t i = lo; i <= hi; i++, n++) {
                float x = from[i % BLOCKS];
                int j = n;
                for (; j > 0 && sorted[j - 1] > x; j--) {
                    sorted[j] = sorted[j - 1];
                }
                sorted[j] = x;
            }
            v = n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
        } else if (how == MOST) {
            v = from[lo % BLOCKS];
            for (int64_t i = lo + 1; i <= hi; i++) {
                v = from[i % BLOCKS] > v ? from[i % BLOCKS] : v;
            }
        } else {
            double s = 0;
            for (int64_t i = lo; i <= hi; i++) {
                s += from[i % BLOCKS];
            }
            v = (float) (s / (double) (hi - lo + 1));
        }
        to->at[b % BLOCKS] = v;
    }
}

/* Work out every block and frame whose inputs are in. */
static void stages(FfwLevel *lv) {
    int64_t nb = blocks(lv);
    stage(lv, lv->lufs, nb, &lv->median, WINDOW, MEDIAN);
    stage(lv, lv->median.at, lv->median.made, &lv->most, SPREAD, MOST);
    stage(lv, lv->most.at, lv->most.made, &lv->slow, SPREAD, MEAN);
    stage(lv, lv->lufs, nb, &lv->q_median, QUICK, MEDIAN);
    stage(lv, lv->q_median.at, lv->q_median.made, &lv->q_most, QUICK, MOST);
    stage(lv, lv->q_most.at, lv->q_most.made, &lv->quick, QUICK, MEAN);

    /* The peak's dilation, a sliding maximum: the deque holds the frames in the window that a later one has not
     * yet topped, their peaks falling from the front, so the front is the highest. */
    int done = ended(lv);
    for (int64_t x; (x = lv->n_dilated) < lv->in && (x + lv->hold < lv->in || done); lv->n_dilated++) {
        int64_t lo = x - lv->hold;
        int64_t hi = x + lv->hold < lv->in ? x + lv->hold : lv->in - 1;
        for (; lv->deque_next <= hi; lv->deque_next++) {
            float p = lv->peak[lv->deque_next % lv->cap];
            while (lv->deque_size > 0
                    && lv->peak[lv->deque[(lv->deque_head + lv->deque_size - 1) % lv->deque_cap] % lv->cap] <= p) {
                lv->deque_size--;
            }
            lv->deque[(lv->deque_head + lv->deque_size) % lv->deque_cap] = lv->deque_next;
            lv->deque_size++;
        }
        while (lv->deque[lv->deque_head] < lo) {
            lv->deque_head = (lv->deque_head + 1) % lv->deque_cap;
            lv->deque_size--;
        }
        lv->dilated[x % lv->cap] = lv->peak[lv->deque[lv->deque_head] % lv->cap];
    }
}

/* A block measure at frame f of block b, straight between the blocks' centres. */
static float between(const FfwLevel *lv, const Stage *s, int64_t b, int64_t nb, int64_t f) {
    float here = s->at[b % BLOCKS];
    float before = b > 0 ? s->at[(b - 1) % BLOCKS] : here;
    float after = b + 1 < nb ? s->at[(b + 1) % BLOCKS] : here;
    double t = ((double) f - ((double) b * lv->block + lv->block / 2.0)) / lv->block;
    return (float) (t < 0 ? here + t * (here - before) : here + t * (after - here));
}

/* Hand on every frame whose measures are worked out, a block at most at a time. */
static int emit(FfwLevel *lv, FfwLevelSink sink, void *to) {
    int64_t nb = blocks(lv);
    int done = ended(lv);
    while (lv->out < lv->in) {
        int64_t from = lv->out;
        int64_t b = from / lv->block;
        int whole = done && lv->slow.made == nb && lv->quick.made == nb;
        if (!whole && !(b + 1 < lv->slow.made && b + 1 < lv->quick.made)) {
            return 1;
        }
        int64_t ready = done && lv->n_dilated == lv->in ? lv->in : lv->n_dilated - lv->ramp;
        int64_t until = (b + 1) * lv->block;
        until = until < lv->in ? until : lv->in;
        until = until < ready ? until : ready;
        if (until <= from) {
            return 1;
        }
        for (int64_t f = from; f < until; f++) {
            float *m = lv->chunk + (size_t) (f - from) * MEASURES;
            m[0] = between(lv, &lv->slow, b, nb, f);
            m[1] = between(lv, &lv->quick, b, nb, f);
            /* The peak: the dilated peaks averaged over ramp either side, a running sum. */
            int64_t lo = f - lv->ramp < 0 ? 0 : f - lv->ramp;
            int64_t hi = f + lv->ramp < lv->n_dilated ? f + lv->ramp : lv->n_dilated - 1;
            while (lv->box_hi < hi) {
                lv->box_hi++;
                lv->box += lv->dilated[lv->box_hi % lv->cap];
            }
            while (lv->box_lo < lo) {
                lv->box -= lv->dilated[lv->box_lo % lv->cap];
                lv->box_lo++;
            }
            m[2] = (float) (lv->box / (double) (hi - lo + 1));
        }
        if (!sink(to, lv->samples + (size_t) (from % lv->cap) * CHANNELS, lv->chunk, until - from)) {
            return 0;
        }
        lv->out = until;
    }
    return 1;
}

FfwLevel *ffwi_level_new(uint32_t rate) {
    FfwLevel *lv = av_mallocz(sizeof *lv);
    if (lv == NULL) {
        return NULL;
    }
    lv->block = rate / 100 > 0 ? (int) (rate / 100) : 1;
    lv->hold = (int) (rate / HOLD);
    lv->ramp = (int) (rate / RAMP);
    lv->cap = (int64_t) BLOCKS * lv->block;
    lv->samples = av_malloc((size_t) lv->cap * CHANNELS * sizeof(float));
    lv->peak = av_malloc((size_t) lv->cap * sizeof(float));
    lv->dilated = av_malloc((size_t) lv->cap * sizeof(float));
    lv->deque_cap = 2 * lv->hold + 2;
    lv->deque = av_malloc((size_t) lv->deque_cap * sizeof(int64_t));
    lv->lufs = av_malloc(BLOCKS * sizeof(float));
    Stage *stages_[] = {&lv->median, &lv->most, &lv->slow, &lv->q_median, &lv->q_most, &lv->quick};
    int ok = lv->samples && lv->peak && lv->dilated && lv->deque && lv->lufs;
    for (int i = 0; i < 6; i++) {
        stages_[i]->at = av_malloc(BLOCKS * sizeof(float));
        ok = ok && stages_[i]->at;
    }
    lv->chunk = av_malloc((size_t) lv->block * MEASURES * sizeof(float));
    if (!ok || lv->chunk == NULL) {
        ffwi_level_free(lv);
        return NULL;
    }
    k_weighting(lv, rate);
    ffwi_level_reset(lv);
    return lv;
}

void ffwi_level_reset(FfwLevel *lv) {
    memset(lv->shelf.z1, 0, sizeof lv->shelf.z1);
    memset(lv->shelf.z2, 0, sizeof lv->shelf.z2);
    memset(lv->pass.z1, 0, sizeof lv->pass.z1);
    memset(lv->pass.z2, 0, sizeof lv->pass.z2);
    lv->sum = 0;
    lv->in = lv->out = 0;
    lv->total = -1;
    lv->median.made = lv->most.made = lv->slow.made = 0;
    lv->q_median.made = lv->q_most.made = lv->quick.made = 0;
    lv->n_dilated = 0;
    lv->deque_head = lv->deque_size = 0;
    lv->deque_next = 0;
    lv->box_lo = 0;
    lv->box_hi = -1;
    lv->box = 0;
}

/* A finished block's loudness, from its K-weighted squares over n frames. */
static void close_block(FfwLevel *lv, int64_t b, int64_t n) {
    double power = lv->sum / (double) n;
    float l = (float) (-0.691 + 10.0 * log10(power + 1e-12));
    lv->lufs[b % BLOCKS] = l < SILENT ? SILENT : l;
    lv->sum = 0;
}

int ffwi_level_push(FfwLevel *lv, const float *src, int64_t n, FfwLevelSink sink, void *to) {
    while (n > 0) {
        /* To the end of the block at most, so the stages run, and frames are handed on, a block at a time: what
         * is pushed never gets more than the lookahead ahead of what is handed on. */
        int64_t k = lv->block - lv->in % lv->block;
        k = k < n ? k : n;
        for (int64_t i = 0; i < k; i++) {
            int64_t f = lv->in + i;
            float *s = lv->samples + (size_t) (f % lv->cap) * CHANNELS;
            float peak = 0;
            for (int c = 0; c < CHANNELS; c++) {
                float x = src != NULL ? src[i * CHANNELS + c] : 0.0f;
                s[c] = x;
                double y = filter(&lv->pass, c, filter(&lv->shelf, c, x));
                lv->sum += y * y;
                float m = fabsf(x);
                peak = m > peak ? m : peak;
            }
            float db = peak > 0 ? 20.0f * log10f(peak) : SILENT;
            lv->peak[f % lv->cap] = db < SILENT ? SILENT : db;
        }
        lv->in += k;
        n -= k;
        if (src != NULL) {
            src += k * CHANNELS;
        }
        if (lv->in % lv->block == 0) {
            close_block(lv, lv->in / lv->block - 1, lv->block);
        }
        stages(lv);
        if (!emit(lv, sink, to)) {
            return 0;
        }
    }
    return 1;
}

int ffwi_level_finish(FfwLevel *lv, FfwLevelSink sink, void *to) {
    if (!ended(lv)) {
        lv->total = lv->in;
        int64_t part = lv->in % lv->block;
        if (part > 0) {
            close_block(lv, lv->in / lv->block, part);
        }
    }
    stages(lv);
    return emit(lv, sink, to);
}

void ffwi_level_free(FfwLevel *lv) {
    if (lv == NULL) {
        return;
    }
    av_free(lv->samples);
    av_free(lv->peak);
    av_free(lv->dilated);
    av_free(lv->deque);
    av_free(lv->lufs);
    av_free(lv->median.at);
    av_free(lv->most.at);
    av_free(lv->slow.at);
    av_free(lv->q_median.at);
    av_free(lv->q_most.at);
    av_free(lv->quick.at);
    av_free(lv->chunk);
    av_free(lv);
}

/* --- the device's half ------------------------------------------------------------------------------------ */

/* From a at 0 to b at 1. */
static float lerp(float a, float b, float t) {
    return a + (b - a) * (t < 0 ? 0 : t > 1 ? 1 : t);
}

/*
 * What the knob stands for, along its way:
 *
 *   0      as stored.
 *   0.25   only the extremes: a band of 6 LU either side of the target left alone, gently past it, nothing
 *          raised, peaks to -3 dBFS. The explosion comes down; the dialogue is as it was.
 *   0.5    night mode: 3:1 all the way to the target, quiet raised up to 12 dB, peaks to -6 dBFS, back up at
 *          20 dB a second.
 *   1      too much, on purpose: everything at the target, raised up to 30 dB (room tone and hiss come up with
 *          it), levelled by the quick loudness so that it pumps on every syllable, back up at 80 dB a second,
 *          peaks to -12 dBFS.
 */
void ffwi_level_knob(FfwLevelKnob *knob, float amount, uint32_t rate) {
    float k = amount > 0.0f ? amount < 1.0f ? amount : 1.0f : 0.0f;     /* NaN is none */
    knob->on = k > 0;
    knob->band = 12.0f * (1.0f - 2.0f * k) > 0 ? 12.0f * (1.0f - 2.0f * k) : 0;
    knob->slope = k < 0.5f ? lerp(0, 2.0f / 3.0f, k * 2) : lerp(2.0f / 3.0f, 1, (k - 0.5f) * 2);
    knob->raise = k < 0.5f ? lerp(0, 12, (k - 0.25f) * 4) : lerp(12, 30, (k - 0.5f) * 2);
    knob->cut = -30.0f * k;
    knob->floor = lerp(-60, -90, (k - 0.5f) * 2);
    knob->ceiling = -12.0f * k;
    knob->quick = lerp(0, 1, (k - 0.75f) * 4);
    knob->release = 5.0f * powf(2.0f, 4.0f * k) / (float) rate;
    knob->guard_release = 30.0f / (float) rate;
}

float ffwi_level_gain(const FfwLevelKnob *knob, const float *measures, FfwLevelHeld *held) {
    float loud = measures[0] + knob->quick * (measures[1] - measures[0]);
    float off = TARGET - loud;
    float level = 0;
    if (off > knob->band) {
        level = (off - knob->band) * knob->slope;
        float most = loud - knob->floor;
        most = most < knob->raise ? most : knob->raise;
        level = level < most ? level : most;
        level = level > 0 ? level : 0;
    } else if (off < -knob->band) {
        level = (off + knob->band) * knob->slope;
        level = level > knob->cut ? level : knob->cut;
    }
    float guard = knob->ceiling - measures[2];
    if (held->started) {
        level = level < held->level + knob->release ? level : held->level + knob->release;
        guard = guard < held->guard + knob->guard_release ? guard : held->guard + knob->guard_release;
    }
    held->started = 1;
    held->level = level;
    held->guard = guard;
    return level < guard ? level : guard;
}
