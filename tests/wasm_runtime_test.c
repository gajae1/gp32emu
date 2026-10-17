/* Compiled with the real freestanding WASM libc, not the host's libm. */
#include "audio/gp32_audio_resampler.h"

#include <stdint.h>

static gp32_audio_resampler_t resampler;
static int16_t input[1024u * 2u];
static int16_t output[8192u * 2u];

__attribute__((export_name("test_resample_constant")))
uint32_t test_resample_constant(uint32_t src_rate, uint32_t dst_rate, int level) {
    for (unsigned i = 0; i < 1024u * 2u; ++i) input[i] = (int16_t)level;
    gp32_audio_resampler_init(&resampler);
    return (uint32_t)gp32_audio_resampler_process(&resampler, input, 1024u,
                                                 src_rate, dst_rate, 0,
                                                 output, 8192u);
}

__attribute__((export_name("test_audio_ptr")))
uint32_t test_audio_ptr(void) { return (uint32_t)(uintptr_t)output; }

__attribute__((export_name("test_coefficient_sum")))
int32_t test_coefficient_sum(uint32_t phase) {
    int32_t sum = 0;
    for (unsigned i = 0; i < GP32_AUDIO_POLY_TAPS; ++i)
        sum += resampler.poly_coef[phase % GP32_AUDIO_POLY_PHASES][i];
    return sum;
}
