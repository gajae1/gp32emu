#include "audio/gp32_audio_resampler.h"

#include <stdint.h>
#include <string.h>

#define GP32_AUDIO_Q32_ONE 4294967296.0
#define GP32_AUDIO_FADE_MAX_FRAMES 96u
#define GP32_AUDIO_FADE_MIN_FRAMES 16u
#define GP32_AUDIO_ADJUST_MIN_PPM (-50000)
#define GP32_AUDIO_ADJUST_MAX_PPM (50000)

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

static int16_t lerp_s16_q32(int16_t a, int16_t b, uint32_t frac) {
    int64_t av = (int64_t)a;
    int64_t dv = (int64_t)b - av;
    return (int16_t)(av + ((dv * (int64_t)frac) >> 32));
}

static int16_t fade_s16(int16_t from, int16_t to, uint32_t num, uint32_t den) {
    int64_t av = (int64_t)from;
    int64_t dv = (int64_t)to - av;
    return (int16_t)(av + (dv * (int64_t)num) / (int64_t)den);
}

void gp32_audio_resampler_init(gp32_audio_resampler_t *r) {
    if (r) memset(r, 0, sizeof(*r));
}

void gp32_audio_resampler_reset(gp32_audio_resampler_t *r) {
    if (!r) return;
    int16_t last_l = r->last_out_l;
    int16_t last_r = r->last_out_r;
    int have_last = r->have_last_out;
    memset(r, 0, sizeof(*r));
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

static void get_sample(const gp32_audio_resampler_t *r, const int16_t *src, size_t input_frames, int have_prev, size_t idx, int16_t *l, int16_t *rr) {
    if (have_prev) {
        if (idx == 0u) { *l = r->prev_l; *rr = r->prev_r; return; }
        idx--;
    }
    if (idx >= input_frames) idx = input_frames ? input_frames - 1u : 0u;
    *l = src[idx * 2u + 0u];
    *rr = src[idx * 2u + 1u];
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

    int have_prev = r->have_prev;
    size_t total_samples = input_frames + (have_prev ? 1u : 0u);
    if (total_samples < 2u) {
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

    /* Peeled prefix: the fade ramp (at most GP32_AUDIO_FADE_MAX_FRAMES outputs
     * after a gap) and the carried sample prev, which is only addressed while
     * phase still sits at virtual index 0.  Borrowing from the previous block
     * stays here so the steady loop below never re-tests it. */
    while (room != 0u && phase < limit_q32 &&
           ((fade_left != 0u && fade_total != 0u) ||
            (have_prev && (uint32_t)(phase >> 32) == 0u))) {
        size_t idx = (size_t)(phase >> 32);
        int16_t l0, r0, l1, r1;
        get_sample(r, src_s16_stereo, input_frames, have_prev, idx, &l0, &r0);
        get_sample(r, src_s16_stereo, input_frames, have_prev, idx + 1u, &l1, &r1);
        int16_t l = lerp_s16_q32(l0, l1, (uint32_t)phase);
        int16_t rr = lerp_s16_q32(r0, r1, (uint32_t)phase);

        if (fade_left && fade_total) {
            uint32_t done = fade_total - fade_left + 1u;
            uint32_t den = fade_total + 1u;
            int16_t from_l = have_last ? last_l : 0;
            int16_t from_r = have_last ? last_r : 0;
            l = fade_s16(from_l, l, done, den);
            rr = fade_s16(from_r, rr, done, den);
            fade_left--;
        }

        dst[0] = l;
        dst[1] = rr;
        dst += 2;
        --room;
        last_l = l;
        last_r = rr;
        have_last = 1;
        phase += step;
    }

    /* Steady state: phase < limit_q32 keeps the integer part at or below
     * intervals - 1, so virtual indices idx and idx + 1 are both real samples
     * of this block and adjacent in memory.  Without prev they are src[idx]
     * and src[idx + 1]; with prev, virtual idx >= 1 maps to src[idx - 1] and
     * src[idx].  Neither case needs a bound test or a prev branch per output. */
    size_t lead = have_prev ? 1u : 0u;
    /* Count the steady outputs once, avoiding two bounds tests per sample.
     * Retain the guarded loop for a step large enough to wrap phase. */
    if (phase < limit_q32 && room && step <= UINT64_MAX - limit_q32) {
        uint64_t need = (limit_q32 - phase - 1u) / step + 1u;
        size_t n = need < (uint64_t)room ? (size_t)need : room;
        /* The prefix has consumed virtual sample zero when prev is present.
         * Bias phase, not the pointer, to keep every address inside src. */
        phase -= (uint64_t)lead << 32;
        do {
            const int16_t *p = src_s16_stereo + (size_t)(phase >> 32) * 2u;
            uint32_t frac = (uint32_t)phase;
            int16_t l = lerp_s16_q32(p[0], p[2], frac);
            int16_t rr = lerp_s16_q32(p[1], p[3], frac);
            dst[0] = l;
            dst[1] = rr;
            dst += 2;
            last_l = l;
            last_r = rr;
            have_last = 1;
            phase += step;
        } while (--n);
        phase += (uint64_t)lead << 32;
    } else {
        while (room != 0u && phase < limit_q32) {
            const int16_t *p = src_s16_stereo + (((size_t)(phase >> 32) - lead) * 2u);
            uint32_t frac = (uint32_t)phase;
            int16_t l = lerp_s16_q32(p[0], p[2], frac);
            int16_t rr = lerp_s16_q32(p[1], p[3], frac);
            dst[0] = l;
            dst[1] = rr;
            dst += 2;
            --room;
            last_l = l;
            last_r = rr;
            have_last = 1;
            phase += step;
        }
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

    r->prev_l = src_s16_stereo[(input_frames - 1u) * 2u + 0u];
    r->prev_r = src_s16_stereo[(input_frames - 1u) * 2u + 1u];
    r->have_prev = 1;
    return (size_t)((dst - dst_s16_stereo) / 2);
}
