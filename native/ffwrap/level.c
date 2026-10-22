/*
 * level.c — dynamic range compression with true lookahead: loud and quiet brought together, the way a TV's
 * night mode does, so the dialogue can be heard without the explosions waking the house.
 *
 * Lookahead without delay. A compressor on a live signal can only see ahead by holding the signal back, and
 * everything else waits with it. A file is not live: the decoder runs ahead of the device by half a second
 * already, so the future is decoded before it is due. This takes the samples the decoder makes, works out each
 * one's gain from the ones around it, before and after, and hands it on once enough of what follows has been
 * decoded. The delay is in the decoder, which is ahead anyway; every sample keeps its place in the stream's time,
 * and the clock (which counts samples played) does not move. After a seek the first sound waits for 0.4 s of
 * stream to be decoded, which takes about a millisecond.
 *
 * Two stages, each giving a gain in dB for every sample frame, the smaller of the two kept:
 *
 *   The leveller, per 10 ms block. A block's loudness is its K-weighted mean square, as ITU-R BS.1770
 *   measures it, and the loudness levelled is the median block's over 0.31 s centred on it: the passage's
 *   sustained level, which a bang or a pause between words does not move (a bang is the guard's to deal with,
 *   and a pause nobody's). The gain pulls it towards TARGET at 3:1, raising quiet passages by up to
 *   MAX_BOOST (less near silence, so room tone stays room tone) and lowering loud ones. The wanted gains are
 *   eroded (each the least over 0.21 s around it) and then averaged over the same span, which gives a curve that
 *   is never above the wanted gain anywhere and that starts to come down before a loud passage arrives, not after.
 *   It comes back up no faster than RELEASE.
 *
 *   The guard, per sample frame: a peak limiter, for what the leveller does not see. A gunshot in a quiet scene
 *   is over before it moves the median, and would otherwise be raised with the scene. Each frame's limit is
 *   the gain that takes its peak to PEAK, a cut where it is above it. Limits are eroded over HOLD either side,
 *   long enough to span a cycle of the lowest bass so the gain does not follow the waveform, then averaged over
 *   RAMP either side, so the gain is down before the peak arrives, and come back up no faster than
 *   PEAK_RELEASE. Each step only ever lowers a gain, so no frame is played above its limit.
 *
 * The gain is in dB so that the device can apply it as far as asked: at compression c, a sample is played at
 * c * gain dB. That scales the curve's 3:1 to a gentler ratio and lets compression change at once, mid-file,
 * with nothing decoded again. At full compression no sample is played above PEAK; at less, none that was below
 * it is raised past it (a raise only gets smaller, and a cut never raises).
 */
#include "level.h"

#include <math.h>
#include <string.h>

#include <libavutil/mem.h>

#define CHANNELS 2

#define TARGET      -18.0   /* LUFS that the leveller pulls towards */
#define SLOPE       (2.0 / 3.0) /* 3:1 */
#define MAX_BOOST   12.0f   /* dB */
#define MAX_CUT     -15.0f  /* dB */
#define FLOOR       -60.0   /* LUFS: a quiet passage at L is raised at most (L - FLOOR) / 2 */
#define WINDOW      15      /* blocks either side of a block that its loudness is measured over */
#define SPREAD      10      /* blocks either side that a wanted gain is eroded over, and averaged over */
#define RELEASE     0.1f    /* dB a block, at most, that the gain comes back up by: 10 dB a second */
#define BLOCKS      128     /* blocks kept: the lookahead (37) and the history (36) fit with room */

#define PEAK        -6.0f   /* dBFS: the guard's ceiling */
#define HOLD        100     /* the guard erodes over 1/HOLD s either side: 10 ms, a whole cycle at 50 Hz in all */
#define RAMP        500     /* and averages over 1/RAMP s either side: 2 ms */
#define PEAK_RELEASE 30.0f  /* dB a second, at most, that the guard's gain comes back up by */

/* A biquad over each channel, direct form II transposed. */
typedef struct {
    double b0, b1, b2, a1, a2;
    double z1[CHANNELS], z2[CHANNELS];
} Biquad;

struct FfwLevel {
    int block;                      /* sample frames a block: 10 ms */
    int hold, ramp;                 /* sample frames either side the guard erodes over, and averages over */
    float release;                  /* dB a sample frame, at most, that the guard's gain comes back up by */
    int64_t cap;                    /* sample frames kept: BLOCKS blocks */

    /* Per sample frame, at f % cap. */
    float *samples;                 /* CHANNELS each, as pushed */
    float *limit;                   /* the gain that takes this frame's peak to PEAK, dB */
    float *eroded;                  /* the least limit within hold of it */
    int64_t *deque;                 /* the frames that may yet be the least in the erosion's window, rising */
    int deque_cap, deque_head, deque_size;
    int64_t deque_next;             /* the next frame to enter the erosion's window */

    /* Per block, at b % BLOCKS. */
    double *power;                  /* K-weighted mean square, channels summed */
    float *want;                    /* the gain its loudness asks for */
    float *settled;                 /* the least want within SPREAD of it */
    float *gain;                    /* the settled gains averaged, and released no faster than RELEASE */

    float *chunk;                   /* a block's gains, handed on */
    Biquad shelf, pass;             /* K-weighting: the head's high shelf, then a high pass */
    double sum;                     /* the block being pushed: its K-weighted squares so far */

    int64_t in, out;                /* sample frames pushed, and handed on, since the reset */
    int64_t total;                  /* sample frames in all once finished, else -1 */
    int64_t n_want, n_settled, n_gain;  /* blocks with each worked out */
    int64_t n_eroded;               /* sample frames with eroded worked out */
    int64_t box_lo, box_hi;         /* the eroded frames summed in box, inclusive */
    double box;
    float guarded;                  /* the guard's last gain, which the next comes up from */
    int has_guarded;
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

/* --- the stages ------------------------------------------------------------------------------------------- */

static int ended(const FfwLevel *lv) {
    return lv->total >= 0;
}

/* Blocks whose power is in: the whole ones, and once finished the last part one too. */
static int64_t blocks(const FfwLevel *lv) {
    return ended(lv) ? (lv->total + lv->block - 1) / lv->block : lv->in / lv->block;
}

/* The gain a block's loudness asks for, from the median power around it. */
static float wanted(double power) {
    double lufs = -0.691 + 10.0 * log10(power + 1e-12);
    double gain = (TARGET - lufs) * SLOPE;
    if (gain > 0) {
        double most = (lufs - FLOOR) / 2;
        if (gain > most) {
            gain = most < 0 ? 0 : most;
        }
        return gain > MAX_BOOST ? MAX_BOOST : (float) gain;
    }
    return gain < MAX_CUT ? MAX_CUT : (float) gain;
}

/* Work out every block and frame whose inputs are in. Each stage needs the one before it `reach` ahead, or, once
 * finished, the rest of it, and looks only as far as there is. */
static void stages(FfwLevel *lv) {
    int64_t nb = blocks(lv);
    int done = ended(lv);
    for (int64_t b; (b = lv->n_want) < nb && (b + WINDOW < nb || done); lv->n_want++) {
        int64_t lo = b - WINDOW < 0 ? 0 : b - WINDOW;
        int64_t hi = b + WINDOW < nb ? b + WINDOW : nb - 1;
        /* The median block: an insertion sort of 31 at most, a hundred times a second. */
        double sorted[2 * WINDOW + 1];
        int n = 0;
        for (int64_t i = lo; i <= hi; i++, n++) {
            double p = lv->power[i % BLOCKS];
            int j = n;
            for (; j > 0 && sorted[j - 1] > p; j--) {
                sorted[j] = sorted[j - 1];
            }
            sorted[j] = p;
        }
        lv->want[b % BLOCKS] = wanted(n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2);
    }
    for (int64_t b; (b = lv->n_settled) < nb && (b + SPREAD < lv->n_want || (done && lv->n_want == nb));
         lv->n_settled++) {
        int64_t lo = b - SPREAD < 0 ? 0 : b - SPREAD;
        int64_t hi = b + SPREAD < lv->n_want ? b + SPREAD : lv->n_want - 1;
        float least = lv->want[lo % BLOCKS];
        for (int64_t i = lo + 1; i <= hi; i++) {
            float w = lv->want[i % BLOCKS];
            least = w < least ? w : least;
        }
        lv->settled[b % BLOCKS] = least;
    }
    for (int64_t b; (b = lv->n_gain) < nb && (b + SPREAD < lv->n_settled || (done && lv->n_settled == nb));
         lv->n_gain++) {
        int64_t lo = b - SPREAD < 0 ? 0 : b - SPREAD;
        int64_t hi = b + SPREAD < lv->n_settled ? b + SPREAD : lv->n_settled - 1;
        double s = 0;
        for (int64_t i = lo; i <= hi; i++) {
            s += lv->settled[i % BLOCKS];
        }
        float g = (float) (s / (double) (hi - lo + 1));
        if (b > 0) {
            float up = lv->gain[(b - 1) % BLOCKS] + RELEASE;
            g = g < up ? g : up;
        }
        lv->gain[b % BLOCKS] = g;
    }
    /* The erosion, a sliding minimum: the deque holds the frames in the window that a later one has not yet
     * undercut, their limits rising from the front, so the front is the least. */
    for (int64_t x; (x = lv->n_eroded) < lv->in && (x + lv->hold < lv->in || done); lv->n_eroded++) {
        int64_t lo = x - lv->hold;
        int64_t hi = x + lv->hold < lv->in ? x + lv->hold : lv->in - 1;
        for (; lv->deque_next <= hi; lv->deque_next++) {
            float l = lv->limit[lv->deque_next % lv->cap];
            while (lv->deque_size > 0
                    && lv->limit[lv->deque[(lv->deque_head + lv->deque_size - 1) % lv->deque_cap] % lv->cap] >= l) {
                lv->deque_size--;
            }
            lv->deque[(lv->deque_head + lv->deque_size) % lv->deque_cap] = lv->deque_next;
            lv->deque_size++;
        }
        while (lv->deque[lv->deque_head] < lo) {
            lv->deque_head = (lv->deque_head + 1) % lv->deque_cap;
            lv->deque_size--;
        }
        lv->eroded[x % lv->cap] = lv->limit[lv->deque[lv->deque_head] % lv->cap];
    }
}

/* Hand on every frame whose gains are worked out, a block at most at a time. */
static int emit(FfwLevel *lv, FfwLevelSink sink, void *to) {
    int64_t nb = blocks(lv);
    int done = ended(lv);
    while (lv->out < lv->in) {
        int64_t from = lv->out;
        int64_t b = from / lv->block;
        if (!(b + 1 < lv->n_gain || (done && lv->n_gain == nb))) {
            return 1;
        }
        int64_t ready = done && lv->n_eroded == lv->in ? lv->in : lv->n_eroded - lv->ramp;
        int64_t until = (b + 1) * lv->block;
        until = until < lv->in ? until : lv->in;
        until = until < ready ? until : ready;
        if (until <= from) {
            return 1;
        }
        float here = lv->gain[b % BLOCKS];
        float before = b > 0 ? lv->gain[(b - 1) % BLOCKS] : here;
        float after = b + 1 < nb ? lv->gain[(b + 1) % BLOCKS] : here;
        double centre = (double) b * lv->block + lv->block / 2.0;
        for (int64_t f = from; f < until; f++) {
            /* The leveller's gain, straight between the blocks' centres. */
            double t = ((double) f - centre) / lv->block;
            float level = (float) (t < 0 ? here + t * (here - before) : here + t * (after - here));
            /* The guard's: the eroded limits averaged over ramp either side, a running sum, and come back up
             * from the last no faster than its release. */
            int64_t lo = f - lv->ramp < 0 ? 0 : f - lv->ramp;
            int64_t hi = f + lv->ramp < lv->n_eroded ? f + lv->ramp : lv->n_eroded - 1;
            while (lv->box_hi < hi) {
                lv->box_hi++;
                lv->box += lv->eroded[lv->box_hi % lv->cap];
            }
            while (lv->box_lo < lo) {
                lv->box -= lv->eroded[lv->box_lo % lv->cap];
                lv->box_lo++;
            }
            float guard = (float) (lv->box / (double) (hi - lo + 1));
            if (lv->has_guarded && guard > lv->guarded + lv->release) {
                guard = lv->guarded + lv->release;
            }
            lv->guarded = guard;
            lv->has_guarded = 1;
            lv->chunk[f - from] = level < guard ? level : guard;
        }
        if (!sink(to, lv->samples + (size_t) (from % lv->cap) * CHANNELS, lv->chunk, until - from)) {
            return 0;
        }
        lv->out = until;
    }
    return 1;
}

/* --- the leveller ----------------------------------------------------------------------------------------- */

FfwLevel *ffwi_level_new(uint32_t rate) {
    FfwLevel *lv = av_mallocz(sizeof *lv);
    if (lv == NULL) {
        return NULL;
    }
    lv->block = rate / 100 > 0 ? (int) (rate / 100) : 1;
    lv->hold = (int) (rate / HOLD);
    lv->ramp = (int) (rate / RAMP);
    lv->release = PEAK_RELEASE / (float) rate;
    lv->cap = (int64_t) BLOCKS * lv->block;
    lv->samples = av_malloc((size_t) lv->cap * CHANNELS * sizeof(float));
    lv->limit = av_malloc((size_t) lv->cap * sizeof(float));
    lv->eroded = av_malloc((size_t) lv->cap * sizeof(float));
    lv->deque_cap = 2 * lv->hold + 2;
    lv->deque = av_malloc((size_t) lv->deque_cap * sizeof(int64_t));
    lv->power = av_malloc(BLOCKS * sizeof(double));
    lv->want = av_malloc(BLOCKS * sizeof(float));
    lv->settled = av_malloc(BLOCKS * sizeof(float));
    lv->gain = av_malloc(BLOCKS * sizeof(float));
    lv->chunk = av_malloc((size_t) lv->block * sizeof(float));
    if (lv->samples == NULL || lv->limit == NULL || lv->eroded == NULL || lv->deque == NULL || lv->power == NULL
            || lv->want == NULL || lv->settled == NULL || lv->gain == NULL || lv->chunk == NULL) {
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
    lv->n_want = lv->n_settled = lv->n_gain = lv->n_eroded = 0;
    lv->box_lo = 0;
    lv->box_hi = -1;
    lv->box = 0;
    lv->deque_head = lv->deque_size = 0;
    lv->deque_next = 0;
    lv->has_guarded = 0;
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
            float l = peak > 0 ? PEAK - 20.0f * log10f(peak) : MAX_BOOST;
            lv->limit[f % lv->cap] = l < MAX_CUT ? MAX_CUT : l > MAX_BOOST ? MAX_BOOST : l;
        }
        lv->in += k;
        n -= k;
        if (src != NULL) {
            src += k * CHANNELS;
        }
        if (lv->in % lv->block == 0) {
            lv->power[(lv->in / lv->block - 1) % BLOCKS] = lv->sum / lv->block;
            lv->sum = 0;
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
            lv->power[(lv->in / lv->block) % BLOCKS] = lv->sum / (double) part;
            lv->sum = 0;
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
    av_free(lv->limit);
    av_free(lv->eroded);
    av_free(lv->deque);
    av_free(lv->power);
    av_free(lv->want);
    av_free(lv->settled);
    av_free(lv->gain);
    av_free(lv->chunk);
    av_free(lv);
}
