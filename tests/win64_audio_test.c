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
 *   2. Submit consumes that flag and marks the gap, so the queued output starts
 *      with the crossfade from the pre-gap sample, while a submit without the
 *      flag does not fade.
 *
 * WIN64_AUDIO_SOURCE can point at a mutated backend copy, which keeps this test
 * honest: the pre-fix arrangement fails the ownership checks below.
 */
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
    CHECK(first_l > 28000 && first_l < 30000, "first queued frame crossfades from the pre-gap sample");
    CHECK(g_audio.queue[ring_at * 2 + 1] == first_l, "right channel crossfades identically");
    /* The gap crossfade chains from the previous output frame, so with silent
     * input it decays without overshoot across the gap window. */
    int nonincreasing = 1;
    for (uint32_t i = 1; i < 48u; ++i) {
        if (g_audio.queue[(ring_at + i) * 2] > g_audio.queue[(ring_at + i - 1u) * 2]) nonincreasing = 0;
    }
    CHECK(nonincreasing, "crossfade decays without overshoot toward the new content");
    CHECK(g_audio.queue[(ring_at + 47u) * 2] <= first_l / 4,
          "crossfade attenuates the pre-gap sample across the gap window");
    CHECK(g_audio.queue[(ring_at + 48u) * 2] == 0 && g_audio.queue[(ring_at + 49u) * 2] == 0,
          "fade window ends and the silent input plays unmodified");
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
