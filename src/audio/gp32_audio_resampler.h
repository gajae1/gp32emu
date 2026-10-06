#ifndef GP32EMU_AUDIO_RESAMPLER_H
#define GP32EMU_AUDIO_RESAMPLER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fractional-interpolation kernel.  Linear interpolation is a triangular
 * reconstruction filter whose sinc^2 response leaves a mirror image of the
 * source band only 20-30 dB down at f_src - f, so upsampling a source that
 * carries energy near its own Nyquist (the GP32 IIS rates are 11025/22050/
 * 23144 Hz, far below the 44100 Hz the frontend wants) lays a continuous
 * broadband image layer over the music.  A windowed-sinc kernel built from
 * 2*half+1 taps at GP32_AUDIO_POLY_PHASES fractional offsets reconstructs the
 * band below min(src, dst)/2 with the images pushed to the Q15 noise floor.
 *
 * The kernel is evaluated causally: output m uses the source frames up to the
 * one it is interpolating, so the band-limited stream trails the linearly
 * interpolated one by GP32_AUDIO_POLY_HALF source frames (1.4 ms at 23144 Hz).
 * Samples older than the current block come from the per-instance history
 * ring, refreshed block by block, so a block boundary is not a filter
 * boundary and the output count and phase model are unchanged. */
#define GP32_AUDIO_POLY_HALF 32u
#define GP32_AUDIO_POLY_TAPS (2u * GP32_AUDIO_POLY_HALF + 1u)
#define GP32_AUDIO_POLY_PHASES 256u
#define GP32_AUDIO_POLY_HISTORY (GP32_AUDIO_POLY_TAPS - 1u)

typedef struct gp32_audio_resampler {
    uint32_t src_rate;
    uint32_t dst_rate;
    uint64_t phase_q32;
    int16_t prev_l;
    int16_t prev_r;
    int have_prev;
    int16_t last_out_l;
    int16_t last_out_r;
    int have_last_out;
    uint32_t fade_left;
    uint32_t fade_total;
    /* Anti-imaging kernel.  poly_src_rate/poly_dst_rate name the rates the
     * coefficients were built for, so clearing them invalidates the table
     * and a plain struct copy carries it.  poly_hist_* ends at the newest
     * source frame seen, i.e. at the sample a later block carries in prev. */
    uint32_t poly_src_rate;
    uint32_t poly_dst_rate;
    int16_t poly_coef[GP32_AUDIO_POLY_PHASES][GP32_AUDIO_POLY_TAPS];
    int16_t poly_hist_l[GP32_AUDIO_POLY_HISTORY];
    int16_t poly_hist_r[GP32_AUDIO_POLY_HISTORY];
} gp32_audio_resampler_t;

void gp32_audio_resampler_init(gp32_audio_resampler_t *r);
void gp32_audio_resampler_reset(gp32_audio_resampler_t *r);
void gp32_audio_resampler_mark_gap(gp32_audio_resampler_t *r, uint32_t dst_rate_hz);
size_t gp32_audio_resampler_max_output_frames(const gp32_audio_resampler_t *r,
                                              size_t input_frames,
                                              uint32_t src_rate_hz,
                                              uint32_t dst_rate_hz,
                                              int32_t rate_adjust_ppm);
/* Exact output count without changing state; SIZE_MAX if Q32 arithmetic
 * cannot represent it. Zero output can still consume input and update carry. */
size_t gp32_audio_resampler_output_frames(const gp32_audio_resampler_t *r,
                                          size_t input_frames,
                                          uint32_t src_rate_hz,
                                          uint32_t dst_rate_hz,
                                          int32_t rate_adjust_ppm);
size_t gp32_audio_resampler_process(gp32_audio_resampler_t *r,
                                    const int16_t *src_s16_stereo,
                                    size_t input_frames,
                                    uint32_t src_rate_hz,
                                    uint32_t dst_rate_hz,
                                    int32_t rate_adjust_ppm,
                                    int16_t *dst_s16_stereo,
                                    size_t dst_cap_frames);
/* Rate-matched delivery: copies input_frames unchanged, but leaves the
 * resampler as a resampler that just emitted them, so a later rate change
 * resumes from a continuous endpoint and a continuous kernel history instead
 * of a hole.  Returns the frames copied. */
size_t gp32_audio_resampler_copy(gp32_audio_resampler_t *r,
                                 const int16_t *src_s16_stereo,
                                 size_t input_frames,
                                 uint32_t rate_hz,
                                 int16_t *dst_s16_stereo);

#ifdef __cplusplus
}
#endif

#endif /* GP32EMU_AUDIO_RESAMPLER_H */
