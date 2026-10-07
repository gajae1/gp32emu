/*
 * Device-free regression for the Win64 WASAPI underrun recovery path.
 *
 * Builds the real src/win64/gp32_win64_audio.c into this TU (source-include
 * pattern of tests/libretro_audio_test.c) and drives the real wasapi_pump()
 * underrun branch through a minimal fake IAudioClient vtable.  No endpoint is
 * opened, no pump thread is started, and no audio device, stream or volume
 * state is touched.
 *
 * Pins the resampler ownership split:
 *   1. The pump thread only raises the locked a->underrun flag: a->resampler is
 *      byte-for-byte unchanged when the underrun branch returns.
 *   2. Submit consumes that flag and marks the gap from the silence the device
 *      heard, so the queued output ramps up from zero instead of stepping to
 *      the last generated sample; a submit without the flag does not fade.
 *
 * WIN64_AUDIO_SOURCE can point at a mutated backend copy, which keeps this test
 * honest: the pre-fix arrangement fails the ownership checks below.
 */
/* mingw-w64 declares the waveOut API as dllimport unless _WINMM_ is set, which
 * would bind the backend's calls straight to libwinmm and bypass the fake
 * driver below.  The backend source is included by this TU, so the switch has
 * to be set before that include. */
#define _WINMM_

#include <stdio.h>
#include <string.h>

#ifndef WIN64_AUDIO_SOURCE
#define WIN64_AUDIO_SOURCE "../src/win64/gp32_win64_audio.c"
#endif
#include WIN64_AUDIO_SOURCE

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
        ++failures; \
    } \
} while (0)

typedef struct fake_client {
    IAudioClient iface;
    UINT32 padding;
} fake_client_t;

static fake_client_t g_client;
static IAudioRenderClient g_render;
static int g_start_calls;
static int g_stop_calls;
static int g_reset_calls;
static int g_client_stopped;
static int g_stop_before_reset;
static int g_render_calls;
static UINT32 g_requested_frames;
static UINT32 g_released_frames;
static int16_t g_render_scratch[8192 * 2];
static gp32_win64_audio_t g_audio;

static HRESULT STDMETHODCALLTYPE fake_get_current_padding(IAudioClient *This, UINT32 *pNumPaddingFrames) {
    CHECK(This == &g_client.iface, "padding queried through the fake client");
    if (pNumPaddingFrames) *pNumPaddingFrames = g_client.padding;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fake_start(IAudioClient *This) { (void)This; ++g_start_calls; return S_OK; }
static HRESULT STDMETHODCALLTYPE fake_stop(IAudioClient *This) { (void)This; ++g_stop_calls; g_client_stopped = 1; return S_OK; }
static HRESULT STDMETHODCALLTYPE fake_reset(IAudioClient *This) {
    (void)This;
    ++g_reset_calls;
    if (g_client_stopped) g_stop_before_reset = 1;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fake_get_buffer(IAudioRenderClient *This, UINT32 NumFramesRequested, BYTE **ppData) {
    (void)This;
    ++g_render_calls;
    g_requested_frames = NumFramesRequested;
    if (!ppData || NumFramesRequested > 8192u) return E_FAIL;
    *ppData = (BYTE *)g_render_scratch;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fake_release_buffer(IAudioRenderClient *This, UINT32 NumFramesWritten, DWORD dwFlags) {
    (void)This; (void)dwFlags;
    ++g_render_calls;
    g_released_frames += NumFramesWritten;
    return S_OK;
}

/* Only the methods the pump can reach are wired; the rest stay NULL. */
static IAudioClientVtbl g_client_vtbl = {
    .GetCurrentPadding = fake_get_current_padding,
    .Start = fake_start,
    .Stop = fake_stop,
    .Reset = fake_reset,
};
static IAudioRenderClientVtbl g_render_vtbl = {
    .GetBuffer = fake_get_buffer,
    .ReleaseBuffer = fake_release_buffer,
};

/* --- Legacy waveOut path -------------------------------------------------
 * waveOut completes whole headers, so the pump must hand the device only the
 * frames the producer really queued.  A chunk shorter than the 1024 frame
 * header is a normal producer hiccup while the other headers keep playing;
 * padding it with the fade-to-silence tail puts fabricated silence in the
 * middle of continuous audio and raises the gap flag, which makes the
 * producer ramp in from silence that was never heard.  The fake driver below
 * records exactly what the pump submits and keeps each written header queued
 * (WHDR_PREPARED without WHDR_DONE) the way a real device does. */
static int g_wave_writes;
static int g_wave_prepare_calls;
static int g_wave_unprepare_calls;
static DWORD g_wave_written_bytes[8];
static int16_t *g_wave_written_data[8];

/* _WINMM_ keeps these declarations out of dllimport form, so these
 * definitions are what the included backend links against. */
MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT hwo, LPWAVEHDR pwh, UINT cbwh) {
    (void)hwo; (void)cbwh;
    ++g_wave_prepare_calls;
    pwh->dwFlags |= WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutWrite(HWAVEOUT hwo, LPWAVEHDR pwh, UINT cbwh) {
    (void)hwo; (void)cbwh;
    if (g_wave_writes < 8) {
        g_wave_written_bytes[g_wave_writes] = pwh->dwBufferLength;
        g_wave_written_data[g_wave_writes] = (int16_t *)pwh->lpData;
    }
    ++g_wave_writes;
    pwh->dwFlags |= WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT hwo, LPWAVEHDR pwh, UINT cbwh) {
    (void)hwo; (void)cbwh;
    ++g_wave_unprepare_calls;
    pwh->dwFlags &= (DWORD)~WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

static void test_waveout_short_read_gap(void) {
    const int16_t sentinel = 0x2b2b; /* value the pump must leave untouched */
    const uint32_t producer_frames = 300u;

    /* waveOut state as waveout_open() leaves it, minus the device handle. */
    g_audio.kind = AUDIO_WAVEOUT;
    g_audio.wave = (HWAVEOUT)(uintptr_t)1;
    for (unsigned i = 0; i < GP32_AUDIO_BUFFERS; ++i) {
        free(g_audio.wave_buf[i]);
        g_audio.wave_buf[i] = (int16_t *)malloc((size_t)GP32_AUDIO_WAVE_FRAMES * 2u * sizeof(int16_t));
        CHECK(g_audio.wave_buf[i] != NULL, "waveOut fixture buffer allocated");
        for (uint32_t f = 0; f < GP32_AUDIO_WAVE_FRAMES * 2u; ++f) g_audio.wave_buf[i][f] = sentinel;
        memset(&g_audio.hdr[i], 0, sizeof(g_audio.hdr[i]));
        g_audio.hdr[i].lpData = (LPSTR)g_audio.wave_buf[i];
        g_audio.hdr[i].dwBufferLength = (DWORD)(GP32_AUDIO_WAVE_FRAMES * 2u * sizeof(int16_t));
    }
    /* The device is mid-stream: one header still holds real queued audio. */
    g_audio.hdr[0].dwFlags = WHDR_PREPARED;

    free(g_audio.queue);
    g_audio.queue = NULL;
    g_audio.read_frame = g_audio.frame_count = g_audio.frame_cap = 0;
    g_audio.underrun = 0;
    g_audio.last_ring_l = g_audio.last_ring_r = 0;
    g_audio.have_last_ring = 0;
    CHECK(ensure_queue(&g_audio, 4096u), "queue capacity for the waveOut fixture");

    static int16_t chunk[300 * 2];
    for (uint32_t i = 0; i < producer_frames; ++i) {
        chunk[i * 2 + 0] = (int16_t)(4000 + (int)i);
        chunk[i * 2 + 1] = (int16_t)(-4000 - (int)i);
    }
    ring_write_bulk_unlocked(&g_audio, chunk, producer_frames);

    g_wave_writes = 0;
    g_wave_prepare_calls = 0;
    g_wave_unprepare_calls = 0;
    CHECK(gp32_win64_audio_pump_backend(&g_audio) == 0, "waveOut short-chunk pump returns 0");

    CHECK(g_wave_writes == 1, "one header takes the short producer chunk");
    CHECK(g_wave_written_bytes[0] == producer_frames * 2u * sizeof(int16_t),
          "the header carries the real frames, not a padded 1024 frame buffer");
    const int16_t *wrote = g_wave_written_data[0];
    int ordered = wrote != NULL;
    for (uint32_t i = 0; ordered && i < producer_frames; ++i) {
        if (wrote[i * 2 + 0] != (int16_t)(4000 + (int)i) ||
            wrote[i * 2 + 1] != (int16_t)(-4000 - (int)i)) ordered = 0;
    }
    CHECK(ordered, "every queued frame reached the device in order");
    int tail_clean = wrote != NULL;
    for (uint32_t i = producer_frames; tail_clean && i < GP32_AUDIO_WAVE_FRAMES; ++i) {
        if (wrote[i * 2 + 0] != sentinel || wrote[i * 2 + 1] != sentinel) tail_clean = 0;
    }
    CHECK(tail_clean, "no fade or silence tail is fabricated past the real frames");
    CHECK(g_audio.wave_buf[2][0] == sentinel &&
          g_audio.wave_buf[2][GP32_AUDIO_WAVE_FRAMES * 2u - 1u] == sentinel,
          "no second header is filled with manufactured silence");
    CHECK(g_audio.frame_count == 0u, "the short chunk was consumed exactly once");
    CHECK(g_audio.underrun == 0, "a short chunk behind queued buffers raises no gap");
    CHECK(g_audio.have_last_ring &&
          g_audio.last_ring_l == (int16_t)(4000 + (int)(producer_frames - 1u)) &&
          g_audio.last_ring_r == (int16_t)(-4000 - (int)(producer_frames - 1u)),
          "the last real frame stays the device continuity anchor");

    /* Steady state still fills whole headers: the pump must not turn the
     * partial-chunk rule into a throughput cap. */
    for (unsigned i = 0; i < GP32_AUDIO_BUFFERS; ++i) {
        g_audio.hdr[i].dwFlags = 0;
        g_audio.hdr[i].lpData = (LPSTR)g_audio.wave_buf[i];
    }
    static int16_t full[2048 * 2];
    for (uint32_t i = 0; i < 2048u; ++i) { full[i * 2 + 0] = 700; full[i * 2 + 1] = -700; }
    ring_write_bulk_unlocked(&g_audio, full, 2048u);
    g_wave_writes = 0;
    g_audio.underrun = 0;
    CHECK(gp32_win64_audio_pump_backend(&g_audio) == 0, "full-chunk waveOut pump returns 0");
    CHECK(g_wave_writes == 2 && g_wave_written_bytes[0] == 1024u * 2u * sizeof(int16_t) &&
          g_wave_written_bytes[1] == 1024u * 2u * sizeof(int16_t),
          "two whole headers are filled while the producer keeps up");
    CHECK(g_audio.frame_count == 0u && g_audio.underrun == 0, "the whole chunk left with no gap");

    /* The device drained everything: this is the gap the fade exists for, so
     * the pump still reports it instead of inventing silence buffers. */
    for (unsigned i = 0; i < GP32_AUDIO_BUFFERS; ++i) {
        g_audio.hdr[i].dwFlags = 0;
        g_audio.hdr[i].lpData = (LPSTR)g_audio.wave_buf[i];
        for (uint32_t f = 0; f < GP32_AUDIO_WAVE_FRAMES * 2u; ++f) g_audio.wave_buf[i][f] = sentinel;
    }
    g_audio.read_frame = 0;
    g_audio.frame_count = 0;
    g_audio.underrun = 0;
    g_wave_writes = 0;
    CHECK(gp32_win64_audio_pump_backend(&g_audio) == 0, "drained waveOut pump returns 0");
    CHECK(g_wave_writes == 0, "an empty ring submits no manufactured silence buffer");
    CHECK(g_audio.underrun == 1, "a device with nothing left to play still raises the gap flag");

    for (unsigned i = 0; i < GP32_AUDIO_BUFFERS; ++i) {
        free(g_audio.wave_buf[i]);
        g_audio.wave_buf[i] = NULL;
        memset(&g_audio.hdr[i], 0, sizeof(g_audio.hdr[i]));
    }
    g_audio.wave = NULL;
    g_audio.kind = AUDIO_WASAPI;
    free(g_audio.queue);
    g_audio.queue = NULL;
    g_audio.read_frame = g_audio.frame_count = g_audio.frame_cap = 0;
}

/* Overflow trims the ring at the read head while the device is between
 * reads: the next frame it pulls must glide from the last frame it actually
 * received instead of stepping across the discard. The resampler's carry
 * belongs to the source stream and must survive the trim. A discard that
 * empties the ring still blends the chunk that refills it. */
static void test_drop_edge_recovery(void) {
    const uint32_t prefill = 32268u;
    const uint32_t in_frames = 1024u;

    /* Fresh ring and producer state; the lock and rates stay as main() set. */
    free(g_audio.queue);
    g_audio.queue = NULL;
    g_audio.read_frame = g_audio.frame_count = g_audio.frame_cap = 0;
    g_audio.underrun = 0;
    g_audio.last_ring_l = g_audio.last_ring_r = 0;
    g_audio.have_last_ring = 0;
    gp32_audio_resampler_init(&g_audio.resampler);

    /* Non-trivial carry, as after real playback. */
    int16_t seed_src[64 * 2];
    int16_t seed_dst[512 * 2];
    for (int i = 0; i < 64; ++i) {
        seed_src[i * 2 + 0] = (int16_t)(i * 8 - 100);
        seed_src[i * 2 + 1] = (int16_t)(100 - i * 8);
    }
    CHECK(gp32_audio_resampler_process(&g_audio.resampler, seed_src, 64, 44100, 48000, 0, seed_dst, 512) > 0,
          "drop fixture resampler seeded");

    /* Wrap-positioned head: after the trim the ramp window crosses the end of
     * the ring allocation. */
    CHECK(ensure_queue(&g_audio, 65536u), "queue capacity for the overflow fixture");
    static int16_t fill[32268 * 2];
    for (uint32_t i = 0; i < prefill; ++i) {
        fill[i * 2 + 0] = 16000;
        fill[i * 2 + 1] = 8000;
    }
    g_audio.read_frame = 64920u;
    ring_write_bulk_unlocked(&g_audio, fill, prefill);
    /* The device last received a large opposite-sign frame: without smoothing
     * the head jump is a 40000-unit step. */
    g_audio.last_ring_l = -24000;
    g_audio.last_ring_r = -24000;
    g_audio.have_last_ring = 1;

    /* Reference resampler sees the same continuation the submit should. */
    static int16_t in[1024 * 2];
    static int16_t ref[2048 * 2];
    for (uint32_t i = 0; i < in_frames; ++i) {
        in[i * 2 + 0] = (int16_t)((int)((i * 37u) % 9000u) - 4500);
        in[i * 2 + 1] = (int16_t)(4500 - (int)((i * 53u) % 9000u));
    }
    gp32_audio_resampler_t clone = g_audio.resampler;
    size_t ref_frames = gp32_audio_resampler_process(&clone, in, in_frames,
                                                      44100, 48000,
                                                      queue_rate_adjust_ppm(prefill),
                                                      ref, 2048);
    CHECK(ref_frames > 0 && ref_frames <= 2048u, "reference continuation produced output");

    gp32_audio_desc_t desc;
    memset(&desc, 0, sizeof desc);
    desc.samples_s16_interleaved = in;
    desc.frame_count = in_frames;
    desc.sample_rate_hz = 44100;
    CHECK(gp32_win64_audio_submit(&g_audio, &desc) == 0, "overflow submit returns 0");
    CHECK(g_audio.frame_count == GP32_AUDIO_MAX_LATENCY_FRAMES,
          "overflow trim kept exactly the latency limit");

    uint32_t fade = g_audio.sample_rate / 1000u;
    uint32_t retained = g_audio.frame_count - (uint32_t)ref_frames;
    CHECK(retained > fade, "trim retained enough frames to keep the ramp inside them");
    CHECK(g_audio.read_frame + fade > g_audio.frame_cap,
          "fixture really exercises a head that wraps the ring end");

    /* The head the device pulls next must start near the last frame it got,
     * not at the retained sample's level. */
    int16_t h0l = g_audio.queue[(size_t)g_audio.read_frame * 2 + 0];
    int16_t h0r = g_audio.queue[(size_t)g_audio.read_frame * 2 + 1];
    int32_t span_l = 16000 - (-24000);
    int32_t span_r = 8000 - (-24000);
    CHECK(h0l > -24000 && h0l <= -24000 + span_l / (int32_t)fade + 2,
          "left head frame starts within one ramp step of the last played sample");
    CHECK(h0r > -24000 && h0r <= -24000 + span_r / (int32_t)fade + 2,
          "right head frame ramps from its own last played sample");

    /* Monotone glide inside the wrapped window, landing on the content level;
     * every retained frame past it is bit-exact untouched. */
    int glide = 1;
    int16_t prev_l = h0l, prev_r = h0r;
    for (uint32_t i = 1; i < fade; ++i) {
        uint32_t at = (g_audio.read_frame + i) % g_audio.frame_cap;
        int16_t l = g_audio.queue[(size_t)at * 2 + 0];
        int16_t rr = g_audio.queue[(size_t)at * 2 + 1];
        if (l < prev_l || rr < prev_r || l > 16000 || rr > 8000 || l < -24000 || rr < -24000) glide = 0;
        prev_l = l;
        prev_r = rr;
    }
    CHECK(glide, "ramp glides without overshoot across the wrapped head");
    CHECK(g_audio.queue[(size_t)((g_audio.read_frame + fade - 1u) % g_audio.frame_cap) * 2] == 16000 &&
          g_audio.queue[(size_t)((g_audio.read_frame + fade - 1u) % g_audio.frame_cap) * 2 + 1] == 8000,
          "ramp lands on the retained content by the window end");
    int tail_ok = 1;
    for (uint32_t i = fade; i < retained; ++i) {
        uint32_t at = (g_audio.read_frame + i) % g_audio.frame_cap;
        if (g_audio.queue[(size_t)at * 2 + 0] != 16000 ||
            g_audio.queue[(size_t)at * 2 + 1] != 8000) { tail_ok = 0; break; }
    }
    CHECK(tail_ok, "every retained frame after the ramp window is untouched");

    /* The whole appended block is the resampler's uninterrupted continuation. */
    int cont_ok = 1;
    for (uint32_t j = 0; j < (uint32_t)ref_frames; ++j) {
        uint32_t at = (g_audio.read_frame + retained + j) % g_audio.frame_cap;
        if (g_audio.queue[(size_t)at * 2 + 0] != ref[j * 2 + 0] ||
            g_audio.queue[(size_t)at * 2 + 1] != ref[j * 2 + 1]) { cont_ok = 0; break; }
    }
    CHECK(cont_ok, "appended output matches an uninterrupted resampler continuation");
    CHECK(g_audio.resampler.phase_q32 == clone.phase_q32 &&
          g_audio.resampler.have_prev == clone.have_prev &&
          g_audio.resampler.prev_l == clone.prev_l && g_audio.resampler.prev_r == clone.prev_r,
          "trim preserved resampler phase and the carried sample");
    CHECK(g_audio.resampler.fade_left == 0 && g_audio.resampler.fade_total == 0,
          "trim armed no resampler fade");

    /* A discard that empties the ring still blends the chunk that refills it:
     * the same arm-then-smooth ordering covers a head that did not exist at
     * trim time. */
    free(g_audio.queue);
    g_audio.queue = NULL;
    g_audio.read_frame = g_audio.frame_count = g_audio.frame_cap = 0;
    gp32_audio_resampler_init(&g_audio.resampler);

    CHECK(ensure_queue(&g_audio, 65536u), "queue capacity for the full-discard fixture");
    static int16_t junk[30000 * 2];
    for (uint32_t i = 0; i < 30000u; ++i) {
        junk[i * 2 + 0] = -5000;
        junk[i * 2 + 1] = -5000;
    }
    ring_write_bulk_unlocked(&g_audio, junk, 30000u);

    /* A submit larger than the whole latency limit forces a full discard, so
     * the post-append head is new content rather than retained frames. */
    static int16_t big[40000 * 2];
    for (uint32_t i = 0; i < 40000u; ++i) {
        big[i * 2 + 0] = 20000;
        big[i * 2 + 1] = 12000;
    }
    desc.samples_s16_interleaved = big;
    desc.frame_count = 40000u;
    CHECK(gp32_win64_audio_submit(&g_audio, &desc) == 0, "full-discard submit returns 0");
    CHECK(g_audio.frame_count > GP32_AUDIO_MAX_LATENCY_FRAMES,
          "the trim discarded every retained frame");
    CHECK(g_audio.read_frame == 0u, "full discard left the head at the new content");

    int16_t b0 = g_audio.queue[0];
    int32_t span_b = 20000 - (-24000);
    CHECK(b0 > -24000 && b0 <= -24000 + span_b / (int32_t)fade + 2,
          "new head blends in from the last played sample after a full discard");
    int mono = 1;
    for (uint32_t i = 1; i < fade; ++i) {
        if (g_audio.queue[(size_t)i * 2] < g_audio.queue[(size_t)(i - 1u) * 2]) mono = 0;
    }
    CHECK(mono, "full-discard ramp is monotonic toward the new content");
    CHECK(g_audio.queue[(size_t)(fade + 10u) * 2] == 20000 &&
          g_audio.queue[(size_t)(fade + 10u) * 2 + 1] == 12000,
          "content past the ramp window is the submitted block");
    CHECK(g_audio.resampler.fade_left == 0 && g_audio.resampler.fade_total == 0,
          "full discard armed no resampler fade");
}

/* Endpoint conversion: GetMixFormat can hand back mono (USB headsets,
 * comms devices) or more than two channels (HDMI receivers, virtual
 * devices). A stereo source owns FL/FR; the remaining endpoint channels
 * must stay silent, and a mono endpoint must carry both source channels
 * instead of dropping the right one. */
static void test_endpoint_channel_conversion(void)
{
    static const int16_t seed[2 * 4] = {
        16384, -8192, 16384, -8192, 16384, -8192, 16384, -8192
    };

    free(g_audio.queue);
    g_audio.queue = NULL;
    g_audio.read_frame = g_audio.frame_count = g_audio.frame_cap = 0;
    g_audio.underrun = 0;
    CHECK(ensure_queue(&g_audio, 64u), "queue capacity for the channel fixture");
    ring_write_bulk_unlocked(&g_audio, seed, 4u);
    CHECK(g_audio.frame_count == 4u, "seeded four stereo frames");

    WAVEFORMATEX six;
    memset(&six, 0, sizeof six);
    six.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    six.nChannels = 6;
    six.nSamplesPerSec = 48000;
    six.wBitsPerSample = 32;
    six.nBlockAlign = 24;
    six.nAvgBytesPerSec = 48000u * 24u;
    g_audio.mixfmt = &six;

    float out6[6 * 4];
    memset(out6, 0x7f, sizeof out6);
    CHECK(fill_wasapi_buffer(&g_audio, (BYTE *)out6, 4u) == 4u, "6ch fill produced every frame");
    CHECK(out6[0] == 0.5f && out6[1] == -0.25f, "6ch float FL/FR carry the stereo source");
    int extra_silent = 1;
    for (int i = 0; i < 4; ++i) {
        for (int c = 2; c < 6; ++c) {
            if (out6[(size_t)i * 6u + (size_t)c] != 0.0f) extra_silent = 0;
        }
    }
    CHECK(extra_silent, "6ch float leaves center/LFE/surround silent");

    ring_write_bulk_unlocked(&g_audio, seed, 4u);
    WAVEFORMATEX mono;
    memset(&mono, 0, sizeof mono);
    mono.wFormatTag = WAVE_FORMAT_PCM;
    mono.nChannels = 1;
    mono.nSamplesPerSec = 48000;
    mono.wBitsPerSample = 16;
    mono.nBlockAlign = 2;
    mono.nAvgBytesPerSec = 48000u * 2u;
    g_audio.mixfmt = &mono;

    int16_t out1[4];
    memset(out1, 0x7f, sizeof out1);
    CHECK(fill_wasapi_buffer(&g_audio, (BYTE *)out1, 4u) == 4u, "mono fill produced every frame");
    int mono_ok = 1;
    for (int i = 0; i < 4; ++i) {
        if (out1[i] != 4096) mono_ok = 0;
    }
    CHECK(mono_ok, "mono endpoint receives the stereo mix, not the left channel alone");

    g_audio.mixfmt = NULL;
    free(g_audio.queue);
    g_audio.queue = NULL;
    g_audio.read_frame = g_audio.frame_count = g_audio.frame_cap = 0;
}

static void test_endpoint_wide_pcm(void) {
    const int16_t seed[] = {32767, -32768, 1, -1};
    const BYTE pcm24[] = {0, 0xff, 0x7f, 0, 0, 0x80, 0, 1, 0, 0, 0xff, 0xff};
    const int32_t pcm32[] = {INT32_C(2147418112), INT32_MIN, 65536, -65536};
    BYTE out[sizeof(pcm32) + 4];
    WAVEFORMATEXTENSIBLE fmt = {0};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.cbSize = 22;
    fmt.Format.nChannels = 2;
    fmt.Format.nSamplesPerSec = 48000;
    fmt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    fmt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    g_audio.mixfmt = &fmt.Format;
    CHECK(ensure_queue(&g_audio, 4u), "wide PCM queue allocated");
    for (unsigned bits = 24; bits <= 32; bits += 8) {
        fmt.Format.wBitsPerSample = (WORD)bits;
        fmt.Samples.wValidBitsPerSample = 24; /* 24 valid bits in either container */
        fmt.Format.nBlockAlign = (WORD)(2u * bits / 8u);
        ring_write_bulk_unlocked(&g_audio, seed, 2);
        memset(out, 0x5a, sizeof(out));
        CHECK(fill_wasapi_buffer(&g_audio, out, 2) == 2, "wide PCM frames delivered");
        size_t bytes = 2u * fmt.Format.nBlockAlign;
        CHECK(memcmp(out, bits == 24 ? (const void *)pcm24 : (const void *)pcm32, bytes) == 0,
              "wide PCM preserves source polarity and level with left-aligned valid bits");
        CHECK(out[bytes] == 0x5a, "wide PCM stops at the negotiated buffer length");
        CHECK(g_audio.frame_count == 0, "wide PCM consumes each source frame once");
    }
    g_audio.mixfmt = NULL;
    free(g_audio.queue);
    g_audio.queue = NULL;
    g_audio.read_frame = g_audio.frame_count = g_audio.frame_cap = 0;
}

int main(void) {
    memset(&g_audio, 0, sizeof g_audio);
    memset(&g_client, 0, sizeof g_client);
    g_client.iface.lpVtbl = &g_client_vtbl;
    g_render.lpVtbl = &g_render_vtbl;

    InitializeCriticalSection(&g_audio.lock);
    g_audio.lock_ready = 1;
    gp32_audio_resampler_init(&g_audio.resampler);
    g_audio.kind = AUDIO_WASAPI;
    g_audio.sample_rate = 48000; /* endpoint mix rate the shared backend adopts */
    g_audio.wasapi_buffer_frames = 4800;
    g_audio.client = &g_client.iface;
    g_audio.render = &g_render;
    g_audio.wasapi_started = 1;
    g_audio.playback_started = 1;

    /* Non-trivial producer state, as it exists after normal playback. */
    int16_t seed_src[64 * 2];
    int16_t seed_dst[512 * 2];
    for (int i = 0; i < 64; ++i) {
        seed_src[i * 2 + 0] = (int16_t)(i * 8 - 100);
        seed_src[i * 2 + 1] = (int16_t)(100 - i * 8);
    }
    size_t seeded = gp32_audio_resampler_process(&g_audio.resampler, seed_src, 64, 44100, 48000, 0, seed_dst, 512);
    CHECK(seeded > 0, "seed resample produced output");
    g_audio.resampler.fade_left = 3;
    g_audio.resampler.fade_total = 96;
    g_audio.resampler.prev_l = 0x1234;
    g_audio.resampler.prev_r = -0x1234;
    /* Pre-gap sample that the recovery crossfade must start from. */
    g_audio.resampler.last_out_l = 30000;
    g_audio.resampler.last_out_r = 30000;
    g_audio.resampler.have_last_out = 1;

    gp32_audio_resampler_t snapshot = g_audio.resampler;

    /* Real wasapi_pump() underrun branch: ring empty, endpoint drained. */
    g_client.padding = 0;
    int pump_rc = gp32_win64_audio_pump_backend(&g_audio);
    CHECK(pump_rc == 0, "underrun pump returns 0");
    CHECK(g_stop_calls == 1 && g_reset_calls == 1, "pump stopped and reset the client exactly once");
    CHECK(g_stop_before_reset == 1, "pump called Stop before Reset");
    CHECK(g_start_calls == 0, "pump did not restart the stream without a prebuffer");
    CHECK(g_audio.underrun == 1, "pump raised the locked underrun flag");
    CHECK(g_audio.playback_started == 0, "pump cleared playback_started");
    CHECK(g_audio.wasapi_started == 0, "pump cleared wasapi_started");
    CHECK(g_audio.frame_count == 0 && g_audio.read_frame == 0, "pump left the queue untouched");
    CHECK(g_render_calls == 0, "underrun branch queued no audio");
    CHECK(g_audio.resampler.prev_l == 0x1234 && g_audio.resampler.prev_r == -0x1234,
          "pump left the resampler interpolation state alone");
    CHECK(g_audio.resampler.fade_left == 3 && g_audio.resampler.fade_total == 96,
          "pump left the resampler fade state alone");
    CHECK(memcmp(&snapshot, &g_audio.resampler, sizeof snapshot) == 0,
          "pump did not mutate the producer-owned resampler");

    /* Producer side: submit consumes the flag and marks the gap. */
    static const int16_t silence[1024 * 2];
    gp32_audio_desc_t desc;
    memset(&desc, 0, sizeof desc);
    desc.samples_s16_interleaved = silence;
    desc.frame_count = 1024;
    desc.sample_rate_hz = 44100;

    uint32_t before_frames = g_audio.frame_count;
    uint32_t before_read = g_audio.read_frame;
    int submit_rc = gp32_win64_audio_submit(&g_audio, &desc);
    CHECK(submit_rc == 0, "submit after the underrun returns 0");
    CHECK(g_audio.underrun == 0, "submit consumed the underrun flag");
    CHECK(g_audio.frame_count > 1024u, "resampled 44.1 -> 48 kHz output was queued");

    uint32_t ring_at = (before_read + before_frames) % g_audio.frame_cap;
    int16_t first_l = g_audio.queue[ring_at * 2 + 0];
    CHECK(first_l > -64 && first_l < 64,
          "resumed head starts from the silence the device heard, not the pre-gap sample");
    CHECK(g_audio.queue[ring_at * 2 + 1] == first_l, "both channels resume from silence identically");
    /* The device heard silence, so the recovery ramp contributes no step of
     * its own: silent input stays silent across the window. */
    int silent = 1;
    for (uint32_t i = 0; i < 48u; ++i)
        if (g_audio.queue[(ring_at + i) * 2] != 0) silent = 0;
    CHECK(silent, "silent input after an underrun stays silent across the recovery window");
    CHECK(g_audio.resampler.fade_left == 0, "gap fade was consumed by the produced block");
    CHECK(g_audio.resampler.src_rate == 44100 && g_audio.resampler.dst_rate == 48000,
          "producer ran the resampler at the real rates");

    /* Negative control: the same pre-gap sample without the flag must not fade. */
    uint32_t control_frames = g_audio.frame_count;
    uint32_t control_read = g_audio.read_frame;
    g_audio.resampler.last_out_l = 30000;
    g_audio.resampler.last_out_r = 30000;
    g_audio.resampler.have_last_out = 1;
    CHECK(g_audio.underrun == 0, "no underrun flag pending before the control submit");
    int control_rc = gp32_win64_audio_submit(&g_audio, &desc);
    CHECK(control_rc == 0, "control submit returns 0");
    uint32_t control_at = (control_read + control_frames) % g_audio.frame_cap;
    CHECK(g_audio.queue[control_at * 2] == 0, "submit without the underrun flag does not fade");

    /* After start, the ring can be empty while the endpoint still holds
     * the prebuffered audio. Its unused space must not become a silence gap. */
    WAVEFORMATEX pcm;
    memset(&pcm, 0, sizeof pcm);
    pcm.wFormatTag = WAVE_FORMAT_PCM;
    pcm.nChannels = 2;
    pcm.nSamplesPerSec = 48000;
    pcm.wBitsPerSample = 16;
    pcm.nBlockAlign = 4;
    pcm.nAvgBytesPerSec = 48000u * 4u;
    g_audio.mixfmt = &pcm;
    g_audio.wasapi_buffer_frames = 4800;
    g_audio.read_frame = 0;
    g_audio.frame_count = 0;
    g_audio.underrun = 0;
    g_audio.last_ring_l = 0;
    g_audio.last_ring_r = 0;
    g_audio.have_last_ring = 0;
    g_audio.playback_started = 0;
    g_audio.wasapi_started = 0;

    static int16_t prebuffer[4096 * 2];
    for (int i = 0; i < 4096; ++i) { prebuffer[i * 2 + 0] = 1000; prebuffer[i * 2 + 1] = -1000; }
    CHECK(ensure_queue(&g_audio, 4096u) == 1, "queue capacity for the prebuffer case");
    ring_write_bulk_unlocked(&g_audio, prebuffer, 4096u);

    g_client.padding = 0;
    g_start_calls = 0; g_stop_calls = 0; g_reset_calls = 0;
    g_render_calls = 0; g_requested_frames = 0; g_released_frames = 0;
    CHECK(gp32_win64_audio_pump_backend(&g_audio) == 0, "prebuffer pump returns 0");
    CHECK(g_start_calls == 1, "prebuffer pump started the stream once");
    CHECK(g_released_frames == 4096u, "prebuffer wrote exactly the queued frames");
    CHECK(g_audio.frame_count == 0u, "prebuffer consumed the whole queue");
    CHECK(g_audio.playback_started == 1, "prebuffer pump entered playback");
    CHECK(g_audio.underrun == 0, "a full prebuffer raises no gap");

    /* Real post-start state: empty ring, endpoint 4096 frames in the 4800 frame buffer. */
    g_client.padding = 4096;
    g_render_calls = 0; g_requested_frames = 0; g_released_frames = 0;
    CHECK(gp32_win64_audio_pump_backend(&g_audio) == 0, "empty-ring pump returns 0");
    CHECK(g_render_calls == 0, "no endpoint buffer is requested without queued audio");
    CHECK(g_released_frames == 0u, "the pump fabricates no silence after a start");
    CHECK(g_audio.underrun == 0, "an empty ring on a running stream raises no gap");

    /* Partial ring: only the real frames may reach the endpoint. */
    static int16_t partial[100 * 2];
    for (int i = 0; i < 100; ++i) { partial[i * 2 + 0] = 12345; partial[i * 2 + 1] = -12345; }
    ring_write_bulk_unlocked(&g_audio, partial, 100u);
    g_audio.last_ring_l = 24000;
    g_audio.last_ring_r = 24000;
    g_audio.have_last_ring = 1;
    for (int i = 100; i < 132; ++i) { g_render_scratch[i * 2 + 0] = -1; g_render_scratch[i * 2 + 1] = -1; }
    g_render_calls = 0; g_requested_frames = 0; g_released_frames = 0;
    CHECK(gp32_win64_audio_pump_backend(&g_audio) == 0, "short-ring pump returns 0");
    CHECK(g_requested_frames == 100u, "the endpoint is asked for exactly the queued frames");
    CHECK(g_released_frames == 100u, "only real queued frames are rendered");
    CHECK(g_render_scratch[0] == 12345 && g_render_scratch[1] == -12345,
          "a rendered frame carries the queued sample");
    CHECK(g_render_scratch[100 * 2 + 0] == -1 && g_render_scratch[100 * 2 + 1] == -1,
          "no fade or silence tail is written past the real frames");
    CHECK(g_audio.underrun == 0, "a short ring raises no gap while the endpoint is buffered");
    CHECK(g_audio.frame_count == 0u, "the real frames were consumed");

    /* A resume that carries real content must still rise from the silence
     * instead of stepping straight to the content level. */
    free(g_audio.queue);
    g_audio.queue = NULL;
    g_audio.read_frame = g_audio.frame_count = g_audio.frame_cap = 0;
    g_audio.underrun = 0;
    g_audio.last_ring_l = g_audio.last_ring_r = 0;
    g_audio.have_last_ring = 0;
    gp32_audio_resampler_init(&g_audio.resampler);
    static int16_t loud[1024 * 2];
    for (int i = 0; i < 1024; ++i) { loud[i * 2 + 0] = 30000; loud[i * 2 + 1] = 30000; }
    gp32_audio_desc_t loud_desc;
    memset(&loud_desc, 0, sizeof loud_desc);
    loud_desc.samples_s16_interleaved = loud;
    loud_desc.frame_count = 1024;
    loud_desc.sample_rate_hz = 44100;
    /* The pump drove the device to silence without consuming content. */
    g_audio.underrun = 1;
    CHECK(gp32_win64_audio_submit(&g_audio, &loud_desc) == 0, "loud resume submit returns 0");
    uint32_t loud_at = g_audio.read_frame;
    int16_t head = g_audio.queue[(size_t)loud_at * 2];
    CHECK(head > -1000 && head < 1000, "loud resume starts near the silence, not at content level");
    int rises = 1;
    for (uint32_t i = 1; i < 48u; ++i)
        if (g_audio.queue[((size_t)loud_at + i) * 2] < g_audio.queue[((size_t)loud_at + i - 1u) * 2]) rises = 0;
    CHECK(rises, "loud resume ramps monotonically up to the content");
    CHECK(g_audio.queue[((size_t)loud_at + 47u) * 2] > 15000,
          "loud resume reaches the content within the recovery window");
    /* Leave the ring empty for the drain-tail section below. */
    free(g_audio.queue);
    g_audio.queue = NULL;
    g_audio.read_frame = g_audio.frame_count = g_audio.frame_cap = 0;
    g_audio.underrun = 0;

    /* Drain tail: the endpoint still holds real frames. A reset now would
     * discard them; the stream must keep playing until padding reaches 0. */
    g_client.padding = 200;
    g_start_calls = 0; g_stop_calls = 0; g_reset_calls = 0;
    g_client_stopped = 0; g_stop_before_reset = 0;
    g_render_calls = 0; g_requested_frames = 0; g_released_frames = 0;
    CHECK(gp32_win64_audio_pump_backend(&g_audio) == 0, "draining pump returns 0");
    CHECK(g_stop_calls == 0 && g_reset_calls == 0,
          "pump does not reset while real frames remain in the endpoint");
    CHECK(g_render_calls == 0, "draining pump requests no endpoint buffer");
    CHECK(g_audio.playback_started == 1 && g_audio.wasapi_started == 1,
          "stream stays started so the trailing frames can play out");
    CHECK(g_audio.underrun == 0, "no gap is raised while real audio still drains");

    /* Endpoint fully drained: the same drain policy resets and re-arms. */
    g_client.padding = 0;
    CHECK(gp32_win64_audio_pump_backend(&g_audio) == 0, "drained pump returns 0");
    CHECK(g_stop_calls == 1 && g_reset_calls == 1, "drained pump stops and resets once");
    CHECK(g_stop_before_reset == 1, "drained pump called Stop before Reset");
    CHECK(g_audio.playback_started == 0 && g_audio.wasapi_started == 0,
          "drained pump re-arms the prebuffer");
    CHECK(g_audio.underrun == 1, "drained pump raises the gap flag");

    test_drop_edge_recovery();
    test_endpoint_channel_conversion();
    test_endpoint_wide_pcm();
    test_waveout_short_read_gap();

    free(g_audio.queue);
    free(g_audio.tmp);
    DeleteCriticalSection(&g_audio.lock);

    if (failures) {
        fprintf(stderr, "win64 audio underrun ownership: %d check(s) failed\n", failures);
        return 1;
    }
    printf("OK win64 audio underrun ownership (resampler untouched by pump, gap handled by submit)\n");
    return 0;
}
