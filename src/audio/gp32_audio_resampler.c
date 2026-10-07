#include "audio/gp32_audio_resampler.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define GP32_AUDIO_Q32_ONE 4294967296.0
#define GP32_AUDIO_FADE_MAX_FRAMES 96u
#define GP32_AUDIO_FADE_MIN_FRAMES 16u
#define GP32_AUDIO_ADJUST_MIN_PPM (-50000)
#define GP32_AUDIO_ADJUST_MAX_PPM (50000)

/* Anti-imaging reconstruction kernel.  The cutoff follows the lower of the
 * two rates, so downsampling (48000 -> 44100 in the win64 frontend) is
 * band-limited below the destination Nyquist as well.  GP32_AUDIO_POLY_FC is
 * the fraction of that Nyquist the passband keeps. */
#define GP32_AUDIO_POLY_BETA 6.0
#define GP32_AUDIO_POLY_FC 0.480
#define GP32_AUDIO_POLY_GAIN 32768.0

static int32_t clamp_ppm(int32_t ppm) {
    if (ppm < GP32_AUDIO_ADJUST_MIN_PPM) return GP32_AUDIO_ADJUST_MIN_PPM;
    if (ppm > GP32_AUDIO_ADJUST_MAX_PPM) return GP32_AUDIO_ADJUST_MAX_PPM;
    return ppm;
}

static uint64_t step_q32(uint32_t src_rate, uint32_t dst_rate, int32_t rate_adjust_ppm) {
    if (!src_rate || !dst_rate) return 0;
    rate_adjust_ppm = clamp_ppm(rate_adjust_ppm);
    double ratio = 1.0 + (double)rate_adjust_ppm / 1000000.0;
    if (ratio < 0.50) ratio = 0.50;
    double den = (double)dst_rate * ratio;
    if (den <= 1.0) den = 1.0;
    uint64_t step = (uint64_t)(((double)src_rate * GP32_AUDIO_Q32_ONE) / den + 0.5);
    return step ? step : 1u;
}

static int16_t fade_s16(int16_t from, int16_t to, uint32_t num, uint32_t den) {
    int64_t av = (int64_t)from;
    int64_t dv = (int64_t)to - av;
    return (int16_t)(av + (dv * (int64_t)num) / (int64_t)den);
}

/* Modified Bessel function of the first kind, order zero, for the Kaiser
 * window.  The series converges in a few dozen terms at the beta used here,
 * and MSVC does not ship I0(). */
static double bessel_i0(double x) {
    double half = x * 0.5;
    double term = 1.0;
    double sum = 1.0;
    for (unsigned k = 1; k <= 48u; ++k) {
        double t = half / (double)k;
        term *= t * t;
        sum += term;
        if (term <= sum * 1e-18) break;
    }
    return sum;
}

static double sinc_pi(double x) {
    if (x == 0.0) return 1.0;
    double px = 3.14159265358979323846 * x;
    return sin(px) / px;
}

static int16_t poly_round_q15(double value) {
    int32_t q = (int32_t)(value * GP32_AUDIO_POLY_GAIN + (value < 0.0 ? -0.5 : 0.5));
    if (q > 32767) q = 32767;
    if (q < -32768) q = -32768;
    return (int16_t)q;
}

/* One table row per fractional phase.  Row p interpolates frac = p/phases
 * and tap i multiplies the source frame i steps older than floor(phase).
 * Normalizing every row to sum to exactly 1.0 in Q15 keeps a constant input
 * an exact constant output; the remainder lands on the largest tap, where it
 * moves one coefficient by at most one LSB. */
static void poly_build_table(gp32_audio_resampler_t *r, uint32_t src_rate, uint32_t dst_rate) {
    uint32_t min_rate = src_rate < dst_rate ? src_rate : dst_rate;
    double fc = GP32_AUDIO_POLY_FC * (double)min_rate / (double)src_rate;
    double i0_beta = bessel_i0(GP32_AUDIO_POLY_BETA);
    for (unsigned p = 0; p < GP32_AUDIO_POLY_PHASES; ++p) {
        double frac = (double)p / (double)GP32_AUDIO_POLY_PHASES;
        int32_t sum = 0;
        unsigned peak = 0;
        int32_t peak_value = INT32_MIN;
        for (unsigned i = 0; i < GP32_AUDIO_POLY_TAPS; ++i) {
            double u = (double)i - (double)GP32_AUDIO_POLY_HALF + frac;
            double a = u / (double)GP32_AUDIO_POLY_HALF;
            int16_t q = 0;
            if (a > -1.0 && a < 1.0) {
                double window = bessel_i0(GP32_AUDIO_POLY_BETA * sqrt(1.0 - a * a)) / i0_beta;
                q = poly_round_q15(2.0 * fc * sinc_pi(2.0 * fc * u) * window);
            }
            r->poly_coef[p][i] = q;
            sum += q;
            if (q > peak_value) { peak_value = q; peak = i; }
        }
        r->poly_coef[p][peak] = (int16_t)((int32_t)r->poly_coef[p][peak] + (32768 - sum));
    }
    r->poly_src_rate = src_rate;
    r->poly_dst_rate = dst_rate;
}

/* Sliding history of the 64 source frames that precede prev, oldest first.
 * After a block the ring must hold the frames before the frame the next block
 * carries in prev, i.e. everything except the block's own final frame. */
static void poly_history_advance(gp32_audio_resampler_t *r, const int16_t *src,
                                 size_t input_frames, int have_prev) {
    size_t block_frames = input_frames - 1u;
    size_t count = (have_prev ? 1u : 0u) + block_frames;
    if (count > GP32_AUDIO_POLY_HISTORY) count = GP32_AUDIO_POLY_HISTORY;
    if (!count) return;
    /* Keep the newest (HISTORY - count) entries: the shift drops the ones the
     * new frames push past 64 frames behind the next prev. */
    size_t kept = GP32_AUDIO_POLY_HISTORY - count;
    if (kept) {
        memmove(r->poly_hist_l, r->poly_hist_l + count, kept * sizeof(int16_t));
        memmove(r->poly_hist_r, r->poly_hist_r + count, kept * sizeof(int16_t));
    }
    size_t at = kept;
    size_t take_src = count;
    /* The block alone covers fewer than count frames only when the count
     * reached back past its first frame, i.e. the previous sample belongs in
     * the middle of the new window. */
    if (have_prev && count > block_frames) {
        r->poly_hist_l[at] = r->prev_l;
        r->poly_hist_r[at] = r->prev_r;
        ++at;
        --take_src;
    }
    size_t first = block_frames - take_src;
    for (size_t i = 0; i < take_src; ++i) {
        r->poly_hist_l[at] = src[(first + i) * 2u + 0u];
        r->poly_hist_r[at] = src[(first + i) * 2u + 1u];
        ++at;
    }
}

/* A stream that starts (or restarts after a reset) has no frames before its
 * first one.  Extending the stream backwards with that first frame keeps a
 * constant input a constant output from the very first sample, exactly as the
 * previous linear interpolator did, instead of fading in over the kernel
 * width.  It also keeps callers that blend toward the head they receive (the
 * libretro declick and the win64 ring trim) free to assume the head is at the
 * stream's own level. */
static void poly_history_prime(gp32_audio_resampler_t *r, const int16_t *src) {
    for (unsigned i = 0; i < GP32_AUDIO_POLY_HISTORY; ++i) {
        r->poly_hist_l[i] = src[0];
        r->poly_hist_r[i] = src[1];
    }
}

/* Band-limited value at the virtual source index n + frac, one filter delay
 * of GP32_AUDIO_POLY_HALF source frames behind the linearly interpolated one.
 * Virtual index 0 is prev; index k >= 1 is src[k - 1] when the block carries a
 * previous sample, and src[k] otherwise.  Taps with a negative index read the
 * ring, the index-zero tap reads prev (or src[0] at the very start) and the
 * rest are contiguous in src, so no tap ever needs a future frame. */
static void poly_gather(const gp32_audio_resampler_t *r, const int16_t *src,
                        int have_prev, size_t n, uint32_t frac,
                        int64_t *acc_l, int64_t *acc_r) {
    const int16_t *c = r->poly_coef[(frac >> 24) & (GP32_AUDIO_POLY_PHASES - 1u)];
    size_t lead = have_prev ? 1u : 0u;
    size_t src_taps = n < GP32_AUDIO_POLY_HISTORY ? n : GP32_AUDIO_POLY_HISTORY;
    /* Each product fits int32_t, but the signed FIR lobes can sum above
     * INT32_MAX on full-scale PCM. Clip only after the wide accumulation. */
    int64_t sum_l = 0, sum_r = 0;
    for (size_t i = 0; i < src_taps; ++i) {
        const int16_t *p = src + ((n - i) - lead) * 2u;
        sum_l += (int32_t)c[i] * (int32_t)p[0];
        sum_r += (int32_t)c[i] * (int32_t)p[1];
    }
    if (n < GP32_AUDIO_POLY_HISTORY) {
        int32_t p0l = have_prev ? (int32_t)r->prev_l : (int32_t)src[0];
        int32_t p0r = have_prev ? (int32_t)r->prev_r : (int32_t)src[1];
        sum_l += (int32_t)c[n] * p0l;
        sum_r += (int32_t)c[n] * p0r;
        for (size_t i = n + 1u; i < GP32_AUDIO_POLY_TAPS; ++i) {
            size_t h = GP32_AUDIO_POLY_HISTORY + n - i;
            sum_l += (int32_t)c[i] * (int32_t)r->poly_hist_l[h];
            sum_r += (int32_t)c[i] * (int32_t)r->poly_hist_r[h];
        }
    }
    *acc_l = sum_l;
    *acc_r = sum_r;
}

static int16_t poly_pack(int64_t acc) {
    int64_t v = (acc + 16384) >> 15;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

void gp32_audio_resampler_init(gp32_audio_resampler_t *r) {
    if (r) memset(r, 0, sizeof(*r));
}

void gp32_audio_resampler_reset(gp32_audio_resampler_t *r) {
    if (!r) return;
    int16_t last_l = r->last_out_l;
    int16_t last_r = r->last_out_r;
    int have_last = r->have_last_out;
    /* Coefficients depend only on their rate keys, not stream history. Keep
     * them across gaps: rebuilding 256 sinc rows delays underrun recovery.
     * process() still rebuilds on a different source/destination rate. */
    memset(r, 0, offsetof(gp32_audio_resampler_t, poly_src_rate));
    memset(r->poly_hist_l, 0, sizeof(r->poly_hist_l));
    memset(r->poly_hist_r, 0, sizeof(r->poly_hist_r));
    r->last_out_l = last_l;
    r->last_out_r = last_r;
    r->have_last_out = have_last;
}

void gp32_audio_resampler_mark_gap(gp32_audio_resampler_t *r, uint32_t dst_rate_hz) {
    if (!r) return;
    gp32_audio_resampler_reset(r);
    uint32_t fade = dst_rate_hz ? dst_rate_hz / 1000u : 48u; /* about 1 ms */
    if (fade < GP32_AUDIO_FADE_MIN_FRAMES) fade = GP32_AUDIO_FADE_MIN_FRAMES;
    if (fade > GP32_AUDIO_FADE_MAX_FRAMES) fade = GP32_AUDIO_FADE_MAX_FRAMES;
    r->fade_left = fade;
    r->fade_total = fade;
}

void gp32_audio_resampler_mark_gap_from_silence(gp32_audio_resampler_t *r,
                                                uint32_t dst_rate_hz) {
    if (!r) return;
    /* Anchor the recovery ramp at zero, the level an emptied ring or stream
     * leaves on the device, before mark_gap preserves that anchor. */
    r->last_out_l = 0;
    r->last_out_r = 0;
    r->have_last_out = 1;
    gp32_audio_resampler_mark_gap(r, dst_rate_hz);
}

size_t gp32_audio_resampler_max_output_frames(const gp32_audio_resampler_t *r,
                                              size_t input_frames,
                                              uint32_t src_rate_hz,
                                              uint32_t dst_rate_hz,
                                              int32_t rate_adjust_ppm) {
    (void)r;
    if (!input_frames || !src_rate_hz || !dst_rate_hz) return 0;
    rate_adjust_ppm = clamp_ppm(rate_adjust_ppm);
    double ratio = 1.0 + (double)rate_adjust_ppm / 1000000.0;
    if (ratio < 0.50) ratio = 0.50;
    double frames = (((double)input_frames + 2.0) * (double)dst_rate_hz * ratio) / (double)src_rate_hz;
    if (frames < 8.0) frames = 8.0;
    if (frames + 32.0 >= (double)SIZE_MAX) return SIZE_MAX;
    return (size_t)(frames + 32.0);
}

static void set_rates(gp32_audio_resampler_t *r, uint32_t src_rate_hz, uint32_t dst_rate_hz) {
    if (!r->src_rate || r->dst_rate != dst_rate_hz) {
        r->phase_q32 = 0;
        r->have_prev = 0;
    } else if (r->src_rate != src_rate_hz) {
        /* Phase is remaining time expressed in source-sample intervals.
         * Convert its units without dropping the carried sample. Split the
         * multiply so ordinary Q32 phases need no wider integer type. */
        uint64_t whole = r->phase_q32 / r->src_rate;
        uint64_t fraction = (r->phase_q32 % r->src_rate) * src_rate_hz / r->src_rate;
        r->phase_q32 = whole > (UINT64_MAX - fraction) / src_rate_hz ?
            UINT64_MAX : whole * src_rate_hz + fraction;
    }
    r->src_rate = src_rate_hz;
    r->dst_rate = dst_rate_hz;
}

size_t gp32_audio_resampler_output_frames(const gp32_audio_resampler_t *r,
                                          size_t input_frames,
                                          uint32_t src_rate_hz,
                                          uint32_t dst_rate_hz,
                                          int32_t rate_adjust_ppm) {
    if (!r || !input_frames || !src_rate_hz || !dst_rate_hz) return 0;
    gp32_audio_resampler_t next = *r;
    set_rates(&next, src_rate_hz, dst_rate_hz);
    uint64_t intervals = (uint64_t)input_frames - (next.have_prev ? 0u : 1u);
    if (intervals > UINT32_MAX) return SIZE_MAX;
    uint64_t limit = intervals << 32;
    if (next.phase_q32 >= limit) return 0;
    uint64_t step = step_q32(src_rate_hz, dst_rate_hz, rate_adjust_ppm);
    if (step > UINT64_MAX - limit) return SIZE_MAX;
    uint64_t count = (limit - next.phase_q32 - 1u) / step + 1u;
    return count > SIZE_MAX ? SIZE_MAX : (size_t)count;
}

size_t gp32_audio_resampler_process(gp32_audio_resampler_t *r,
                                    const int16_t *src_s16_stereo,
                                    size_t input_frames,
                                    uint32_t src_rate_hz,
                                    uint32_t dst_rate_hz,
                                    int32_t rate_adjust_ppm,
                                    int16_t *dst_s16_stereo,
                                    size_t dst_cap_frames) {
    if (!r || !src_s16_stereo || !dst_s16_stereo || !input_frames || !src_rate_hz || !dst_rate_hz || !dst_cap_frames) return 0;
    set_rates(r, src_rate_hz, dst_rate_hz);
    if (r->poly_src_rate != src_rate_hz || r->poly_dst_rate != dst_rate_hz)
        poly_build_table(r, src_rate_hz, dst_rate_hz);

    int have_prev = r->have_prev;
    if (!have_prev) poly_history_prime(r, src_s16_stereo);
    size_t total_samples = input_frames + (have_prev ? 1u : 0u);
    if (total_samples < 2u) {
        poly_history_advance(r, src_s16_stereo, input_frames, have_prev);
        r->prev_l = src_s16_stereo[(input_frames - 1u) * 2u + 0u];
        r->prev_r = src_s16_stereo[(input_frames - 1u) * 2u + 1u];
        r->have_prev = 1;
        return 0;
    }

    size_t intervals = total_samples - 1u;
    uint64_t limit_q32 = (uint64_t)intervals << 32;
    uint64_t step = step_q32(src_rate_hz, dst_rate_hz, rate_adjust_ppm);
    uint64_t phase = r->phase_q32;
    uint32_t fade_left = r->fade_left;
    uint32_t fade_total = r->fade_total;
    int have_last = r->have_last_out;
    int16_t last_l = r->last_out_l;
    int16_t last_r = r->last_out_r;
    int16_t *dst = dst_s16_stereo;
    size_t room = dst_cap_frames;

    /* Gap ramp first: at most GP32_AUDIO_FADE_MAX_FRAMES outputs that glide
     * from the last delivered sample onto the band-limited stream. */
    while (room != 0u && phase < limit_q32 && fade_left != 0u && fade_total != 0u) {
        int64_t acc_l, acc_r;
        poly_gather(r, src_s16_stereo, have_prev, (size_t)(phase >> 32),
                    (uint32_t)phase, &acc_l, &acc_r);
        int16_t l = poly_pack(acc_l);
        int16_t rr = poly_pack(acc_r);
        uint32_t done = fade_total - fade_left + 1u;
        uint32_t den = fade_total + 1u;
        int16_t from_l = have_last ? last_l : 0;
        int16_t from_r = have_last ? last_r : 0;
        l = fade_s16(from_l, l, done, den);
        rr = fade_s16(from_r, rr, done, den);
        fade_left--;
        dst[0] = l;
        dst[1] = rr;
        dst += 2;
        --room;
        last_l = l;
        last_r = rr;
        have_last = 1;
        phase += step;
    }

    /* Steady state.  phase < limit_q32 keeps floor(phase) at or below
     * intervals - 1, so every virtual index the kernel reads is a real frame
     * of this block or of the history ring. */
    while (room != 0u && phase < limit_q32) {
        int64_t acc_l, acc_r;
        poly_gather(r, src_s16_stereo, have_prev, (size_t)(phase >> 32),
                    (uint32_t)phase, &acc_l, &acc_r);
        int16_t l = poly_pack(acc_l);
        int16_t rr = poly_pack(acc_r);
        dst[0] = l;
        dst[1] = rr;
        dst += 2;
        --room;
        last_l = l;
        last_r = rr;
        have_last = 1;
        phase += step;
    }

    r->fade_left = fade_left;
    r->last_out_l = last_l;
    r->last_out_r = last_r;
    r->have_last_out = have_last;

    if (phase >= limit_q32) r->phase_q32 = phase - limit_q32;
    else {
        /* Output capacity was too small.  Do not carry a phase beyond this
         * block without retaining the full input; clamp to the final interval
         * instead of risking a bad next-block index.  Normal callers use the
         * max-output helper and should not hit this path. */
        r->phase_q32 = 0;
    }

    poly_history_advance(r, src_s16_stereo, input_frames, have_prev);
    r->prev_l = src_s16_stereo[(input_frames - 1u) * 2u + 0u];
    r->prev_r = src_s16_stereo[(input_frames - 1u) * 2u + 1u];
    r->have_prev = 1;
    return (size_t)((dst - dst_s16_stereo) / 2);
}

size_t gp32_audio_resampler_copy(gp32_audio_resampler_t *r,
                                 const int16_t *src_s16_stereo,
                                 size_t input_frames,
                                 uint32_t rate_hz,
                                 int16_t *dst_s16_stereo) {
    if (!r || !src_s16_stereo || !dst_s16_stereo || !input_frames || !rate_hz) return 0;
    memcpy(dst_s16_stereo, src_s16_stereo, input_frames * 2u * sizeof(int16_t));

    /* Rate-matched delivery: the samples pass through unchanged, but the
     * resampler stays a resampler that has just emitted them, so a later rate
     * change resumes from a continuous endpoint and kernel history.  The
     * table key is left alone: it still names whatever rates the coefficients
     * were built for, and any other rate rebuilds it on demand. */
    if (!r->have_prev) poly_history_prime(r, src_s16_stereo);
    poly_history_advance(r, src_s16_stereo, input_frames, r->have_prev);

    r->src_rate = rate_hz;
    r->dst_rate = rate_hz;
    r->phase_q32 = UINT64_C(1) << 32;
    r->prev_l = src_s16_stereo[(input_frames - 1u) * 2u + 0u];
    r->prev_r = src_s16_stereo[(input_frames - 1u) * 2u + 1u];
    r->have_prev = 1;
    r->last_out_l = r->prev_l;
    r->last_out_r = r->prev_r;
    r->have_last_out = 1;
    r->fade_left = 0;
    r->fade_total = 0;
    return input_frames;
}
